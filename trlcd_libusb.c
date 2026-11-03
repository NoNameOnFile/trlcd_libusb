// trlcd_libusb.c — USB LCD compositor with PNG/APNG/Video, UTF-8 TTF text, overlays,
// robust USB retries, big FB + viewport. APNG frames are precomposed with
// correct blend/dispose and timed playback. Video support via FFmpeg.
//
// Build:
//   gcc -O2 -Wall trlcd_libusb.c lodepng.c -lusb-1.0 -lavformat -lavcodec -lavutil -lswscale -lm -o trlcd_libusb
//
// Requires (in same directory):
//   - stb_image.h        (PNG decode for static images)
//   - stb_truetype.h     (TTF text rasterization)
//   - lodepng.h, lodepng.c (APNG frame decode)
//   - FFmpeg libraries   (libavformat, libavcodec, libavutil, libswscale)
//
// Notes:
//   * Background offset can be numeric or "center" (layout.cfg)
//   * Text is TTF-only; bitmap font removed.
//   * Tokens: %CPU_TEMP% %CPU_USAGE% %MEM_USED% %MEM_FREE% %GPU_TEMP% %GPU_USAGE% %TIME% %DATE% %FAN_RPM% %FAN_SPEED% %CPU_FREQ% %UPTIME% %LOAD_AVG%
//   * Video backgrounds supported via FFmpeg (MP4, AVI, MOV, etc.)

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <math.h>

#include <signal.h>

// FFmpeg includes
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>

// FFmpeg compatibility
#ifndef AV_PIX_FMT_RGBA
#define AV_PIX_FMT_RGBA AV_PIX_FMT_RGB32
#endif

static volatile sig_atomic_t g_reload = 0;
static volatile sig_atomic_t g_stop   = 0;

static void on_sighup(int sig){ (void)sig; g_reload = 1; }
static void on_sigterm(int sig){ (void)sig; g_stop = 1; }

// Panel params
#define VID 0x0416
#define PID 0x5302
#define W   240
#define H   320
#define PACK 512
#define FRAME_LEN (W*H*2)
#define CL_TIMEOUT 1000

// HID class control: Host->Interface | Class | Interface
#define BMRT  0x21
#define BREQ  0x09          // SET_REPORT
#define WVALUE 0x0200       // (ReportType<<8)|ReportID = Output, ID 0

static void die(const char* m){ perror(m); exit(1); }

// stb_image (PNG) ----------------------------------------------------------------
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#include "stb_image.h"

// stb_truetype (TTF) -------------------------------------------------------------
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// LodePNG (APNG) -----------------------------------------------------------------
#include "lodepng.h"

// Utils --------------------------------------------------------------------------

static int decode_png_rgba_lenient(const unsigned char* png, size_t pngsz,
                                   unsigned char** out, unsigned* w, unsigned* h){
    *out = NULL; *w = *h = 0;

    // First try LodePNG's decode32 (RGBA)
    unsigned err = lodepng_decode32(out, w, h, png, pngsz);
    if(!err) return 0;

    // Fallback to stb_image
    int tw=0, th=0, comp=0;
    unsigned char* stb = stbi_load_from_memory(png, (int)pngsz, &tw, &th, &comp, 4);
    if(stb){
        *out = stb; *w = (unsigned)tw; *h = (unsigned)th;
        return 0;
    }

    fprintf(stderr, "apng: decode frame failed %u: %s\n", err, lodepng_error_text(err));
    return (int)err;
}

static inline uint32_t be32r(const unsigned char *p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|((uint32_t)p[3]);
}
static inline void be32w(unsigned char *p, uint32_t v){
    p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16);
    p[2]=(unsigned char)(v>>8);  p[3]=(unsigned char)(v);
}
static uint64_t now_monotonic_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000ull + (uint64_t)ts.tv_nsec/1000000ull;
}
static void trim(char *s){
    int n=(int)strlen(s);
    while(n>0 && (s[n-1]=='\r'||s[n-1]=='\n'||isspace((unsigned char)s[n-1]))) s[--n]=0;
    int i=0; while(s[i] && isspace((unsigned char)s[i])) i++;
    if(i>0) memmove(s,s+i,strlen(s+i)+1);
}

// Layout & Config ----------------------------------------------------------------
typedef struct { int x,y,w,h; uint8_t r,g,b,a; } Overlay;

typedef enum { ORIENT_PORTRAIT=0, ORIENT_LANDSCAPE=1 } UiOrient;

typedef struct {
    char *text;
    int x,y;
    uint8_t r,g,b,a;

    int orient_override;        // -1 inherit, 0 P, 1 L
    int landscape_ccw_override; // -1 inherit, 0 CW, 1 CCW
    int flip_override;          // -1 inherit, 0 no, 1 yes

    char *ttf_path;
    int   ttf_px;
} TextItem;

typedef struct {
    char *path;
    int x,y;
    int alpha;          // 0..255
    float scale;        // 1.0 default
    // APNG controls
    double apng_speed;  // 1.0
    int64_t apng_start_ms; // 0
    int apng_loop_mode; // 0=default(file) 1=infinite 2=once 3=customN
    int apng_loop_N;
} ImgLayer;

typedef struct {
    // Background
    char background_png[512];
    char background_video[512];  // Video file path
    int  background_flip;   // 0|1
    int  bg_x_mode;         // 0 numeric, 1 center
    int  bg_y_mode;         // 0 numeric, 1 center
    int  bg_x, bg_y;

    // Global text orientation
    UiOrient text_orient;
    int  text_flip;             // 0|1
    int  text_landscape_ccw;    // 0 CW, 1 CCW

    // FB & viewport
    int fb_scale_percent;       // >=100
    int viewport_x;             // -1 center
    int viewport_y;             // -1 center

    // Streaming
    int fps;
    int once;
    int iface;
    
    // Compression (removed - LCD only supports RGB565)

    // Objects
    Overlay  *overlays; int n_overlays;
    TextItem *texts;    int n_texts;
    ImgLayer *imgs;     int n_imgs;

    // Global TTF default
    char default_ttf[512];
    int  default_ttf_px;

    // Background APNG controls
    double bg_apng_speed;
    int64_t bg_apng_start_ms;
    int bg_apng_loop_mode; // 0 default(file) 1 inf 2 once 3 customN
    int bg_apng_loop_N;

    // Background video controls
    double bg_video_speed;
    int64_t bg_video_start_ms;
    int bg_video_loop_mode; // 0 default(file) 1 inf 2 once 3 customN
    int bg_video_loop_N;
    int video_cache_enabled; // Enable video caching for short videos
} Layout;

static UiOrient parse_orient(const char *v){
    if(v && strcasecmp(v,"landscape")==0) return ORIENT_LANDSCAPE;
    return ORIENT_PORTRAIT;
}
static int parse_rect(const char *s, int *x,int *y,int *w,int *h){
    int X=0,Y=0,Wd=0,Ht=0;
    int n = sscanf(s," %d , %d , %d , %d ",&X,&Y,&Wd,&Ht);
    if(n!=4) return -1; 
    *x=X; *y=Y; *w=Wd; *h=Ht; 
    return 0;
}
static int parse_rgbA(const char *s, uint8_t *r,uint8_t *g,uint8_t *b,uint8_t *a){
    int R=0,G=0,B=0,A=255;
    int n = sscanf(s," %d , %d , %d , %d ",&R,&G,&B,&A);
    if(n<3) return -1;
    if(R<0||R>255||G<0||G>255||B<0||B>255) return -1;
    if(n==4){ if(A<0||A>255) return -1; *a=(uint8_t)A; } else *a=255;
    *r=(uint8_t)R; *g=(uint8_t)G; *b=(uint8_t)B; return 0;
}
static int parse_bool_inherit(const char *v, int *out){
    if(!v) return -1;
    if(strcasecmp(v,"inherit")==0){ *out=-1; return 0; }
    if(!strcasecmp(v,"0")||!strcasecmp(v,"false")||!strcasecmp(v,"no")){ *out=0; return 0; }
    if(!strcasecmp(v,"1")||!strcasecmp(v,"true")||!strcasecmp(v,"yes")){ *out=1; return 0; }
    char *e=NULL; long n=strtol(v,&e,10); if(e && *e==0){ *out=(n!=0); return 0; }
    return -1;
}
static void layout_init(Layout *L){
    memset(L,0,sizeof(*L));
    L->fps=0; L->once=1; L->iface=-1;
    L->background_flip=0;
    L->bg_x_mode=1; L->bg_y_mode=1; // center default
    L->text_orient=ORIENT_PORTRAIT;
    L->text_flip=0;
    L->text_landscape_ccw=0;
    // Compression settings removed - LCD only supports RGB565
    L->fb_scale_percent=150;
    L->viewport_x=-1; L->viewport_y=-1;
    L->default_ttf[0]=0; L->default_ttf_px=0;
    L->bg_apng_speed=1.0; L->bg_apng_start_ms=0; L->bg_apng_loop_mode=0; L->bg_apng_loop_N=0;
    L->bg_video_speed=1.0; L->bg_video_start_ms=0; L->bg_video_loop_mode=1; L->bg_video_loop_N=0; // default infinite loop
    L->video_cache_enabled=1; // Enable video caching by default
}
static void add_overlay(Layout *L, Overlay ov){
    L->overlays=(Overlay*)realloc(L->overlays,(L->n_overlays+1)*sizeof(Overlay));
    if(!L->overlays) die("realloc overlays"); 
    L->overlays[L->n_overlays++]=ov;
}
static void add_text(Layout *L, TextItem ti){
    L->texts=(TextItem*)realloc(L->texts,(L->n_texts+1)*sizeof(TextItem));
    if(!L->texts) die("realloc texts"); 
    L->texts[L->n_texts++]=ti;
}
static void add_img(Layout *L, ImgLayer im){
    L->imgs=(ImgLayer*)realloc(L->imgs,(L->n_imgs+1)*sizeof(ImgLayer));
    if(!L->imgs) die("realloc imgs"); 
    L->imgs[L->n_imgs++]=im;
}
static int load_layout(const char *path, Layout *L){
    layout_init(L);
    FILE *f=fopen(path,"r"); if(!f){ perror("open layout.cfg"); return -1; }
    enum { SEC_NONE, SEC_OVERLAY, SEC_TEXT, SEC_IMAGE } sec=SEC_NONE;
    TextItem cur_text; memset(&cur_text,0,sizeof(cur_text));
    cur_text.a=255; cur_text.orient_override=-1;
    cur_text.landscape_ccw_override=-1; cur_text.flip_override=-1;
    int have_text=0;

    ImgLayer cur_img; memset(&cur_img,0,sizeof cur_img);
    cur_img.alpha=255; cur_img.scale=1.0f;
    cur_img.apng_speed=1.0; cur_img.apng_start_ms=0; cur_img.apng_loop_mode=0; cur_img.apng_loop_N=0;
    int have_img=0;

    char line[1024];
    while(fgets(line,sizeof line,f)){
        trim(line); if(line[0]==0||line[0]=='#') continue;
        if(line[0]=='['){
            if(sec==SEC_TEXT && have_text && cur_text.text){ add_text(L,cur_text); memset(&cur_text,0,sizeof cur_text);
                cur_text.a=255; cur_text.orient_override=-1; cur_text.landscape_ccw_override=-1; cur_text.flip_override=-1; have_text=0; }
            if(sec==SEC_IMAGE && have_img && cur_img.path){ add_img(L,cur_img); memset(&cur_img,0,sizeof cur_img);
                cur_img.alpha=255; cur_img.scale=1.0f; cur_img.apng_speed=1.0; cur_img.apng_start_ms=0; cur_img.apng_loop_mode=0; cur_img.apng_loop_N=0; have_img=0; }
            if(!strcmp(line,"[overlay]")) sec=SEC_OVERLAY;
            else if(!strcmp(line,"[text]")) sec=SEC_TEXT;
            else if(!strcmp(line,"[image]")) sec=SEC_IMAGE;
            else sec=SEC_NONE;
            continue;
        }
        char *eq=strchr(line,'='); if(!eq) continue; *eq=0; char *k=line,*v=eq+1; trim(k); trim(v);

        if(sec==SEC_NONE){
            if(!strcmp(k,"background_png")) { strncpy(L->background_png,v,sizeof(L->background_png)-1); }
            else if(!strcmp(k,"background_video")) { strncpy(L->background_video,v,sizeof(L->background_video)-1); }
            else if(!strcmp(k,"background_flip")) L->background_flip=atoi(v);
            else if(!strcmp(k,"background_x")){ 
                // Strip comments from value
                char *comment = strchr(v, '#');
                if(comment) *comment = 0;
                // Trim whitespace
                while(*v && (*v == ' ' || *v == '\t')) v++;
                char *end = v + strlen(v) - 1;
                while(end > v && (*end == ' ' || *end == '\t')) *end-- = 0;
                
                if(strcasecmp(v,"center")==0)L->bg_x_mode=1; else{L->bg_x_mode=0; L->bg_x=atoi(v);} 
            }
            else if(!strcmp(k,"background_y")){ 
                // Strip comments from value
                char *comment = strchr(v, '#');
                if(comment) *comment = 0;
                // Trim whitespace
                while(*v && (*v == ' ' || *v == '\t')) v++;
                char *end = v + strlen(v) - 1;
                while(end > v && (*end == ' ' || *end == '\t')) *end-- = 0;
                
                if(strcasecmp(v,"center")==0)L->bg_y_mode=1; else{L->bg_y_mode=0; L->bg_y=atoi(v);} 
            }

            else if(!strcmp(k,"text_orientation")) L->text_orient=parse_orient(v);
            else if(!strcmp(k,"text_flip")) L->text_flip=atoi(v);
            else if(!strcmp(k,"text_landscape_dir")) L->text_landscape_ccw=(strcasecmp(v,"ccw")==0)?1:0;

            else if(!strcmp(k,"fb_scale_percent")) L->fb_scale_percent=atoi(v);
            else if(!strcmp(k,"viewport_x")) {
                // Strip comments from value
                char *comment = strchr(v, '#');
                if(comment) *comment = 0;
                // Trim trailing whitespace
                char *end = v + strlen(v) - 1;
                while(end > v && (*end == ' ' || *end == '\t')) *end-- = 0;
                
                L->viewport_x=(strcasecmp(v,"center")==0)?-1:atoi(v);
            }
            else if(!strcmp(k,"viewport_y")) {
                // Strip comments from value
                char *comment = strchr(v, '#');
                if(comment) *comment = 0;
                // Trim trailing whitespace
                char *end = v + strlen(v) - 1;
                while(end > v && (*end == ' ' || *end == '\t')) *end-- = 0;
                
                L->viewport_y=(strcasecmp(v,"center")==0)?-1:atoi(v);
            }

            else if(!strcmp(k,"fps")) L->fps=atoi(v);
            else if(!strcmp(k,"once")) L->once=atoi(v);
            else if(!strcmp(k,"iface")) L->iface=atoi(v);
            // Compression config removed - LCD only supports RGB565

            else if(!strcmp(k,"default_ttf")) { strncpy(L->default_ttf,v,sizeof(L->default_ttf)-1); }
            else if(!strcmp(k,"default_ttf_px")) L->default_ttf_px=atoi(v);

            // background APNG tuning
            else if(!strcmp(k,"apng_speed")) L->bg_apng_speed=atof(v);
            else if(!strcmp(k,"apng_start_ms")) L->bg_apng_start_ms=(int64_t)atoll(v);
            else if(!strcmp(k,"apng_loop")){
                if(!strcasecmp(v,"default")){ L->bg_apng_loop_mode=0; }
                else if(!strcasecmp(v,"infinite")){ L->bg_apng_loop_mode=1; }
                else if(!strcasecmp(v,"once")){ L->bg_apng_loop_mode=2; }
                else { L->bg_apng_loop_mode=3; L->bg_apng_loop_N=atoi(v); if(L->bg_apng_loop_N<0)L->bg_apng_loop_N=0; }
            }

            // background video tuning
            else if(!strcmp(k,"video_speed")) L->bg_video_speed=atof(v);
            else if(!strcmp(k,"video_start_ms")) L->bg_video_start_ms=(int64_t)atoll(v);
            else if(!strcmp(k,"video_loop")){
                if(!strcasecmp(v,"default")){ L->bg_video_loop_mode=0; }
                else if(!strcasecmp(v,"infinite")){ L->bg_video_loop_mode=1; }
                else if(!strcasecmp(v,"once")){ L->bg_video_loop_mode=2; }
                else { L->bg_video_loop_mode=3; L->bg_video_loop_N=atoi(v); if(L->bg_video_loop_N<0)L->bg_video_loop_N=0; }
            }
            else if(!strcmp(k,"video_cache_enabled")) L->video_cache_enabled=atoi(v);

        } else if(sec==SEC_OVERLAY){
            static Overlay ov;
            if(!strcmp(k,"rect")){
                if(parse_rect(v,&ov.x,&ov.y,&ov.w,&ov.h)!=0) fprintf(stderr,"Bad overlay rect\n");
            } else if(!strcmp(k,"color")){
                if(parse_rgbA(v,&ov.r,&ov.g,&ov.b,&ov.a)!=0) fprintf(stderr,"Bad overlay color\n");
                add_overlay(L,ov); memset(&ov,0,sizeof ov);
            }

        } else if(sec==SEC_TEXT){
            have_text=1;
            if(!strcmp(k,"text")){ if(cur_text.text) free(cur_text.text); cur_text.text=strdup(v); }
            else if(!strcmp(k,"x")) cur_text.x=atoi(v);
            else if(!strcmp(k,"y")) cur_text.y=atoi(v);
            else if(!strcmp(k,"color")){ if(parse_rgbA(v,&cur_text.r,&cur_text.g,&cur_text.b,&cur_text.a)!=0) fprintf(stderr,"Bad text color\n"); }
            else if(!strcmp(k,"orientation")){
                if(!strcasecmp(v,"inherit")) cur_text.orient_override=-1;
                else if(!strcasecmp(v,"portrait")) cur_text.orient_override=0;
                else if(!strcasecmp(v,"landscape")) cur_text.orient_override=1;
                else fprintf(stderr,"[text] orientation must be portrait|landscape|inherit\n");
            } else if(!strcmp(k,"landscape_dir")){
                if(!strcasecmp(v,"inherit")) cur_text.landscape_ccw_override=-1;
                else if(!strcasecmp(v,"cw")) cur_text.landscape_ccw_override=0;
                else if(!strcasecmp(v,"ccw")) cur_text.landscape_ccw_override=1;
                else fprintf(stderr,"[text] landscape_dir must be cw|ccw|inherit\n");
            } else if(!strcmp(k,"flip")){
                int tmp; if(parse_bool_inherit(v,&tmp)==0) cur_text.flip_override=tmp;
                else fprintf(stderr,"[text] flip must be 0|1|true|false|yes|no|inherit\n");
            } else if(!strcmp(k,"ttf")){
                if(cur_text.ttf_path) free(cur_text.ttf_path);
                cur_text.ttf_path=strdup(v);
            } else if(!strcmp(k,"ttf_px")){
                cur_text.ttf_px=atoi(v);
            }

        } else if(sec==SEC_IMAGE){
            have_img=1;
            if(!strcmp(k,"path")){ if(cur_img.path) free(cur_img.path); cur_img.path=strdup(v); }
            else if(!strcmp(k,"x")) cur_img.x=atoi(v);
            else if(!strcmp(k,"y")) cur_img.y=atoi(v);
            else if(!strcmp(k,"alpha")) cur_img.alpha=atoi(v);
            else if(!strcmp(k,"scale")) cur_img.scale=(float)atof(v);
            else if(!strcmp(k,"apng_speed")) cur_img.apng_speed=atof(v);
            else if(!strcmp(k,"apng_start_ms")) cur_img.apng_start_ms=(int64_t)atoll(v);
            else if(!strcmp(k,"apng_loop")){
                if(!strcasecmp(v,"default")){ cur_img.apng_loop_mode=0; }
                else if(!strcasecmp(v,"infinite")){ cur_img.apng_loop_mode=1; }
                else if(!strcasecmp(v,"once")){ cur_img.apng_loop_mode=2; }
                else { cur_img.apng_loop_mode=3; cur_img.apng_loop_N=atoi(v); if(cur_img.apng_loop_N<0) cur_img.apng_loop_N=0; }
            }
        }
    }
    if(sec==SEC_TEXT && have_text && cur_text.text){ add_text(L,cur_text); }
    else { if(cur_text.text) free(cur_text.text); if(cur_text.ttf_path) free(cur_text.ttf_path); }
    if(sec==SEC_IMAGE && have_img && cur_img.path){ add_img(L,cur_img); }
    else if(cur_img.path){ free(cur_img.path); }
    fclose(f);

    if(L->background_png[0]==0 && L->background_video[0]==0){ fprintf(stderr,"layout.cfg missing 'background_png=' or 'background_video='\n"); return -1; }
    if(L->fb_scale_percent<100) L->fb_scale_percent=100;
    if(L->bg_apng_speed<=0) L->bg_apng_speed=1.0;
    if(L->bg_video_speed<=0) L->bg_video_speed=1.0;
    for(int i=0;i<L->n_imgs;i++){ if(L->imgs[i].apng_speed<=0) L->imgs[i].apng_speed=1.0; }

    return 0;
}

// Token Metrics ------------------------------------------------------------------
typedef struct {
    int have_temp; float temp_c;
    int have_usage; float usage_pct; uint64_t prev_idle, prev_total; int prev_valid;
    int have_mem; unsigned long long mem_total_kb, mem_avail_kb;
    int have_gpu_temp; float gpu_temp_c;
    int have_gpu_usage; float gpu_usage_pct;
    int have_fan_rpm; int fan_rpm;
    int have_fan_speed; float fan_speed_pct;
    int have_disk_usage; float disk_usage_pct;
    int have_disk_free; unsigned long long disk_free_gb;
    int have_cpu_freq; float cpu_freq_ghz;
    int have_uptime; unsigned long uptime_sec;
    int have_load_avg; float load_avg_1m;
    char time_hhmm[8];
    char date_ymd[32];
    // Optimization: cache timestamps
    uint64_t last_update_ms;
    int cache_valid;
} Metrics;

static int read_file_ll(const char *path, long long *out){
    FILE *f=fopen(path,"r"); if(!f) return -1;
    char buf[128]; if(!fgets(buf,sizeof buf,f)){ fclose(f); return -1; }
    fclose(f); char *e=NULL; errno=0; long long v=strtoll(buf,&e,10); if(errno) return -1; *out=v; return 0;
}
static int read_file_u64(const char *path, unsigned long long *out){
    FILE *f=fopen(path,"r"); if(!f) return -1;
    unsigned long long v=0; if(fscanf(f,"%llu",&v)!=1){ fclose(f); return -1; } fclose(f); *out=v; return 0;
}
static int get_cpu_temp_c(float *out_c){
    long long best=-1;
    
    // First try hwmon sensors (prefer k10temp for AMD CPUs)
    for(int h=0;h<16;h++)for(int t=1;t<=8;t++){ 
        char p[160]; 
        snprintf(p,sizeof p,"/sys/class/hwmon/hwmon%d/temp%d_input",h,t);
        long long v; 
        if(read_file_ll(p,&v)==0){ 
            if(v>1000) v=v/1000; // Convert millidegrees to degrees
            if(v>best) best=v; 
        } 
    }
    
    // Fallback to thermal zones if no hwmon sensors found
    if(best < 0) {
        for(int i=0;i<32;i++){ 
            char p[128]; 
            snprintf(p,sizeof p,"/sys/class/thermal/thermal_zone%d/temp",i);
            long long v; 
            if(read_file_ll(p,&v)==0){ 
                if(v>1000) v=v/100; // Convert centidegrees to degrees
                if(v>best) best=v; 
            } 
        }
    }
    
    if(best<0) return -1; 
    *out_c = best; 
    return 0;
}
static int read_cpu_totals(uint64_t *idle, uint64_t *total){
    FILE *f=fopen("/proc/stat","r"); if(!f) return -1; char line[256];
    if(!fgets(line,sizeof line,f)){ fclose(f); return -1; } fclose(f);
    unsigned long long u=0,n=0,s=0,i=0,w=0,irq=0,sirq=0,st=0;
    int c=sscanf(line,"cpu %llu %llu %llu %llu %llu %llu %llu %llu",&u,&n,&s,&i,&w,&irq,&sirq,&st);
    if(c<4) return -1; 
    *idle = i+w; 
    *total = u+n+s+*idle+irq+sirq+st; 
    return 0;
}
static int get_mem_total_avail_kb(unsigned long long *tot_kb, unsigned long long *avail_kb){
    FILE *f=fopen("/proc/meminfo","r"); if(!f) return -1; char key[64]; unsigned long long val=0; char unit[16];
    unsigned long long total=0,avail=0;
    while(fscanf(f,"%63[^:]: %llu %15s\n",key,&val,unit)==3){ if(!strcmp(key,"MemTotal")) total=val; else if(!strcmp(key,"MemAvailable")) avail=val; }
    fclose(f); if(total==0||avail==0) return -1; *tot_kb=total; *avail_kb=avail; return 0;
}
static int get_gpu_temp_c(float *out_c){
    const char *names[]={"amdgpu","nvidia","nouveau","i915","xe"}; float best=-1.0f;
    for(int h=0;h<32;h++){
        char namep[128]; snprintf(namep,sizeof namep,"/sys/class/hwmon/hwmon%d/name",h);
        FILE *nf=fopen(namep,"r"); if(!nf) continue; char nm[64]={0}; if(!fgets(nm,sizeof nm,nf)){ fclose(nf); continue; } fclose(nf);
        for(char *p=nm;*p;p++) if(*p=='\n'||*p=='\r') *p=0; 
        int ok=0;
        for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) if(!strcasecmp(nm,names[i])){ ok=1; break; }
        if(!ok) continue;
        for(int t=1;t<=8;t++){ char tp[160]; snprintf(tp,sizeof tp,"/sys/class/hwmon/hwmon%d/temp%d_input",h,t);
            long long v; if(read_file_ll(tp,&v)==0){ float c=(v>=1000)?(v/1000.0f):(v/1.0f); if(c>best) best=c; } }
    }
    if(best<0) return -1; 
    *out_c=best; 
    return 0;
}
static int get_gpu_usage_pct(float *out_pct){
    DIR *d=opendir("/sys/class/drm"); if(!d) return -1; struct dirent *de; float best=-1.0f;
    while((de=readdir(d))){ if(strncmp(de->d_name,"card",4)!=0) continue; char p1[512]; unsigned long long v=0;
        snprintf(p1,sizeof p1,"/sys/class/drm/%s/device/gpu_busy_percent",de->d_name);
        if(read_file_u64(p1,&v)==0){ if((float)v>best) best=(float)v; continue; }
        snprintf(p1,sizeof p1,"/sys/class/drm/%s/device/busy_percent",de->d_name);
        if(read_file_u64(p1,&v)==0){ if((float)v>best) best=(float)v; continue; }
        snprintf(p1,sizeof p1,"/sys/class/drm/%s/device/gt_busy_percent",de->d_name);
        if(read_file_u64(p1,&v)==0){ if((float)v>best) best=(float)v; continue; }
    }
    closedir(d); if(best<0) return -1; if(best>100.0f) best=100.0f; *out_pct=best; return 0;
}

// New metric collection functions
static int get_fan_rpm(int *out_rpm){
    int best_rpm = -1;
    for(int h=0; h<16; h++){
        for(int f=1; f<=8; f++){
            char path[160];
            snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/fan%d_input", h, f);
            long long rpm;
            if(read_file_ll(path, &rpm) == 0 && rpm > 0){
                if(rpm > best_rpm) best_rpm = (int)rpm;
            }
        }
    }
    if(best_rpm < 0) return -1;
    *out_rpm = best_rpm;
    return 0;
}

static int get_fan_speed_pct(float *out_pct){
    int best_pct = -1;
    for(int h=0; h<16; h++){
        for(int f=1; f<=8; f++){
            char path[160];
            snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/pwm%d", h, f);
            long long pwm;
            if(read_file_ll(path, &pwm) == 0 && pwm >= 0){
                // PWM is typically 0-255, convert to percentage
                int pct = (int)((pwm * 100) / 255);
                if(pct > best_pct) best_pct = pct;
            }
        }
    }
    if(best_pct < 0) return -1;
    *out_pct = (float)best_pct;
    return 0;
}

static int get_cpu_freq_ghz(float *out_ghz){
    FILE *f = fopen("/proc/cpuinfo", "r");
    if(!f) return -1;
    
    char line[256];
    float max_freq = 0.0f;
    while(fgets(line, sizeof line, f)){
        if(strncmp(line, "cpu MHz", 7) == 0){
            float freq_mhz;
            if(sscanf(line, "cpu MHz : %f", &freq_mhz) == 1){
                float freq_ghz = freq_mhz / 1000.0f;
                if(freq_ghz > max_freq) max_freq = freq_ghz;
            }
        }
    }
    fclose(f);
    
    if(max_freq <= 0) return -1;
    *out_ghz = max_freq;
    return 0;
}

static int get_uptime_sec(unsigned long *out_sec){
    FILE *f = fopen("/proc/uptime", "r");
    if(!f) return -1;
    
    double uptime;
    if(fscanf(f, "%lf", &uptime) == 1){
        *out_sec = (unsigned long)uptime;
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

static int get_load_avg_1m(float *out_load){
    FILE *f = fopen("/proc/loadavg", "r");
    if(!f) return -1;
    
    float load1, load5, load15;
    if(fscanf(f, "%f %f %f", &load1, &load5, &load15) == 3){
        *out_load = load1;
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}
static void metrics_init(Metrics *m){ memset(m,0,sizeof *m); }
static void fmt_bytes_short(unsigned long long bytes, char out[16]){
    const char *u[]={"B","K","M","G","T","P"}; double v=(double)bytes; int idx=0;
    while(v>=1024.0 && idx<5){ v/=1024.0; idx++; }
    if(v>=100.0) snprintf(out,16,"%.0f%s",v,u[idx]);
    else if(v>=10.0) snprintf(out,16,"%.1f%s",v,u[idx]);
    else snprintf(out,16,"%.2f%s",v,u[idx]);
}
static void update_metrics(Metrics *m, int blocking_initial){
    uint64_t now_ms = now_monotonic_ms();
    
    // Always update time/date (lightweight)
    time_t now=time(NULL); struct tm lt; localtime_r(&now,&lt);
    snprintf(m->time_hhmm,sizeof m->time_hhmm,"%02d:%02d", lt.tm_hour, lt.tm_min);
    snprintf(m->date_ymd,sizeof m->date_ymd,"%04d-%02d-%02d", lt.tm_year+1900, lt.tm_mon+1, lt.tm_mday);

    // Cache expensive system calls for 1 second
    if(blocking_initial || !m->cache_valid || (now_ms - m->last_update_ms) > 1000){
        float tc; if(get_cpu_temp_c(&tc)==0){ m->have_temp=1; m->temp_c=tc; }

        uint64_t idle=0,total=0;
        if(read_cpu_totals(&idle,&total)==0){
            if(!m->prev_valid){
                if(blocking_initial){
                    struct timespec ts={0,60*1000*1000}; nanosleep(&ts,NULL);
                    uint64_t i2=0,t2=0; if(read_cpu_totals(&i2,&t2)==0){
                        uint64_t did=i2-idle, dtt=t2-total; if(dtt>0){
                            float used=(float)(dtt-did)*100.0f/(float)dtt; if(used<0)used=0; if(used>100)used=100;
                            m->usage_pct=used; m->have_usage=1;
                        }
                        m->prev_idle=i2; m->prev_total=t2; m->prev_valid=1;
                    }
                } else { m->prev_idle=idle; m->prev_total=total; m->prev_valid=1; }
            } else {
                uint64_t did=idle-m->prev_idle, dtt=total-m->prev_total; if(dtt>0){
                    float used=(float)(dtt-did)*100.0f/(float)dtt; if(used<0)used=0; if(used>100)used=100;
                    m->usage_pct=used; m->have_usage=1;
                }
                m->prev_idle=idle; m->prev_total=total;
            }
        }
        unsigned long long tot=0, avail=0;
        if(get_mem_total_avail_kb(&tot,&avail)==0){ m->mem_total_kb=tot; m->mem_avail_kb=avail; m->have_mem=1; }

        float gtc; if(get_gpu_temp_c(&gtc)==0){ m->gpu_temp_c=gtc; m->have_gpu_temp=1; }
        float gup; if(get_gpu_usage_pct(&gup)==0){ if(gup<0)gup=0; if(gup>100)gup=100; m->gpu_usage_pct=gup; m->have_gpu_usage=1; }

        // New metrics
        int fan_rpm; if(get_fan_rpm(&fan_rpm)==0){ m->fan_rpm=fan_rpm; m->have_fan_rpm=1; }
        float fan_speed; if(get_fan_speed_pct(&fan_speed)==0){ m->fan_speed_pct=fan_speed; m->have_fan_speed=1; }
        float cpu_freq; if(get_cpu_freq_ghz(&cpu_freq)==0){ m->cpu_freq_ghz=cpu_freq; m->have_cpu_freq=1; }
        unsigned long uptime; if(get_uptime_sec(&uptime)==0){ m->uptime_sec=uptime; m->have_uptime=1; }
        float load_avg; if(get_load_avg_1m(&load_avg)==0){ m->load_avg_1m=load_avg; m->have_load_avg=1; }
        
        m->last_update_ms = now_ms;
        m->cache_valid = 1;
    }
}
static void expand_tokens(char *out, size_t outsz, const char *in, const Metrics *m){
    size_t oi=0;
    for(size_t i=0; in[i] && oi+1<outsz; ){
        if(in[i]=='%'){
            const char *start=in+i+1; const char *end=strchr(start,'%');
            if(end){
                size_t len=(size_t)(end-start); char tok[64]={0};
                if(len<sizeof(tok)){
                    for(size_t k=0;k<len;k++){ char c=start[k]; tok[k]=(char)((c>='a'&&c<='z')?(c-32):c); }
                    tok[len]=0; char repl[64]={0}; int replaced=0;
                    if(!strcmp(tok,"CPU_TEMP")){ if(m&&m->have_temp){ int t10=(int)(m->temp_c*10+0.5f); int w=t10/10,d=t10%10; if(d==0) snprintf(repl,64,"%d°C",w); else snprintf(repl,64,"%d.%d°C",w,d);} else snprintf(repl,64,"N/A"); replaced=1; }
                    else if(!strcmp(tok,"CPU_USAGE")){ if(m&&m->have_usage){ int p=(int)(m->usage_pct+0.5f); if(p<0)p=0; if(p>100)p=100; snprintf(repl,64,"%d%%",p);} else snprintf(repl,64,"N/A"); replaced=1; }
                    else if(!strcmp(tok,"MEM_USED")){ if(m&&m->have_mem){ unsigned long long used_kb=(m->mem_total_kb>m->mem_avail_kb)?(m->mem_total_kb-m->mem_avail_kb):0ULL; char s[16]; fmt_bytes_short(used_kb*1024ULL,s); snprintf(repl,64,"%s",s);} else snprintf(repl,64,"N/A"); replaced=1; }
                    else if(!strcmp(tok,"MEM_FREE")){ if(m&&m->have_mem){ char s[16]; fmt_bytes_short(m->mem_avail_kb*1024ULL,s); snprintf(repl,64,"%s",s);} else snprintf(repl,64,"N/A"); replaced=1; }
                    else if(!strcmp(tok,"GPU_TEMP")){ if(m&&m->have_gpu_temp){ int t=(int)(m->gpu_temp_c+0.5f); snprintf(repl,64,"%d°C",t);} else snprintf(repl,64,"N/A"); replaced=1; }
                    else if(!strcmp(tok,"GPU_USAGE")){ if(m&&m->have_gpu_usage){ int p=(int)(m->gpu_usage_pct+0.5f); if(p<0)p=0; if(p>100)p=100; snprintf(repl,64,"%d%%",p);} else snprintf(repl,64,"N/A"); replaced=1; }
                    else if(!strcmp(tok,"TIME")){ snprintf(repl,64,"%s",(m&&m->time_hhmm[0])?m->time_hhmm:"N/A"); replaced=1; }
                    else if(!strcmp(tok,"DATE")){ snprintf(repl,64,"%s",(m&&m->date_ymd[0])?m->date_ymd:"N/A"); replaced=1; }
                    else if(!strcmp(tok,"FAN_RPM")){ if(m&&m->have_fan_rpm){ snprintf(repl,64,"%d RPM",m->fan_rpm);} else snprintf(repl,64,"N/A"); replaced=1; }
                    else if(!strcmp(tok,"FAN_SPEED")){ if(m&&m->have_fan_speed){ int p=(int)(m->fan_speed_pct+0.5f); if(p<0)p=0; if(p>100)p=100; snprintf(repl,64,"%d%%",p);} else snprintf(repl,64,"N/A"); replaced=1; }
                    else if(!strcmp(tok,"CPU_FREQ")){ if(m&&m->have_cpu_freq){ snprintf(repl,64,"%.2f GHz",m->cpu_freq_ghz);} else snprintf(repl,64,"N/A"); replaced=1; }
                    else if(!strcmp(tok,"UPTIME")){ if(m&&m->have_uptime){ unsigned long days=m->uptime_sec/86400; unsigned long hours=(m->uptime_sec%86400)/3600; unsigned long mins=(m->uptime_sec%3600)/60; if(days>0) snprintf(repl,64,"%lud %luh %lum",days,hours,mins); else if(hours>0) snprintf(repl,64,"%luh %lum",hours,mins); else snprintf(repl,64,"%lum",mins);} else snprintf(repl,64,"N/A"); replaced=1; }
                    else if(!strcmp(tok,"LOAD_AVG")){ if(m&&m->have_load_avg){ snprintf(repl,64,"%.2f",m->load_avg_1m);} else snprintf(repl,64,"N/A"); replaced=1; }
                    if(replaced){ size_t rl=strlen(repl); for(size_t r=0;r<rl && oi+1<outsz;r++) out[oi++]=repl[r]; i+=(len+2); continue; }
                }
            }
        }
        out[oi++]=in[i++];
    }
    out[oi]=0;
}

// RGBA / Blitting ---------------------------------------------------------------
static void rotate180_rgba(uint8_t *buf, int w, int h){
    size_t px=(size_t)w*h; for(size_t i=0,j=px-1;i<j;i++,j--){ uint8_t t0=buf[i*4+0],t1=buf[i*4+1],t2=buf[i*4+2],t3=buf[i*4+3];
        buf[i*4+0]=buf[j*4+0]; buf[i*4+1]=buf[j*4+1]; buf[i*4+2]=buf[j*4+2]; buf[i*4+3]=buf[j*4+3];
        buf[j*4+0]=t0; buf[j*4+1]=t1; buf[j*4+2]=t2; buf[j*4+3]=t3; }
}
static void premultiply_rgba(uint8_t *px, int w, int h){
    size_t n=(size_t)w*h;
    for(size_t i=0;i<n;i++){
        uint8_t *p=px+i*4; uint16_t a=p[3];
        p[0]=(uint8_t)((p[0]*a + 127)/255);
        p[1]=(uint8_t)((p[1]*a + 127)/255);
        p[2]=(uint8_t)((p[2]*a + 127)/255);
    }
}
// dst and src premultiplied
static inline void over_premul(uint8_t *dst, const uint8_t *src){
    uint32_t aS=src[3], inv=255-aS;
    dst[0]=(uint8_t)(src[0] + (dst[0]*inv + 127)/255);
    dst[1]=(uint8_t)(src[1] + (dst[1]*inv + 127)/255);
    dst[2]=(uint8_t)(src[2] + (dst[2]*inv + 127)/255);
    dst[3]=(uint8_t)(aS     + (dst[3]*inv + 127)/255);
}
static uint8_t* fb_rgba_alloc_clear(int fbw, int fbh){
    uint8_t *fb=(uint8_t*)calloc((size_t)fbw*fbh,4); if(!fb) die("calloc fb"); return fb;
}

// JPEG compression removed - LCD does not support JPEG format

// RLE compression removed - LCD does not support RLE format

static void blit_png_into_fb(uint8_t *fb,int fbw,int fbh,const uint8_t *src,int sw,int sh,int dstx,int dsty,int alpha,float scale){
    if(scale<=0.0f) scale=1.0f;
    int outw=(int)(sw*scale), outh=(int)(sh*scale);
    for(int y=0;y<outh;y++){
        int sy=(int)((y/scale)+0.5f); if(sy<0) sy=0; if(sy>=sh) sy=sh-1;
        for(int x=0;x<outw;x++){
            int sx=(int)((x/scale)+0.5f); if(sx<0) sx=0; if(sx>=sw) sx=sw-1;
            int dx=dstx+x, dy=dsty+y;
            if((unsigned)dx>=(unsigned)fbw || (unsigned)dy>=(unsigned)fbh) continue;
            const uint8_t *s=src+4*(sy*sw+sx);
            uint8_t sp[4]={s[0],s[1],s[2],s[3]};
            if(alpha>=0 && alpha<255){
                sp[0]=(uint8_t)((sp[0]*alpha + 127)/255);
                sp[1]=(uint8_t)((sp[1]*alpha + 127)/255);
                sp[2]=(uint8_t)((sp[2]*alpha + 127)/255);
                sp[3]=(uint8_t)((sp[3]*alpha + 127)/255);
            }
            over_premul(fb + 4*(dy*fbw + dx), sp);
        }
    }
}

// UI mapping (portrait/landscape + flip) ----------------------------------------
static inline void map_ui_xy_fb(int xL,int yL, UiOrient o,int flip180, int *dx,int *dy, int fbw,int fbh, const Layout *L){
    int mx,my;
    if(o==ORIENT_PORTRAIT){ mx=xL; my=yL; }
    else{
        if(L->text_landscape_ccw){ mx=W-1-yL; my=xL; } // 90° CCW
        else{ mx=yL; my=H-1-xL; }                      // 90° CW
    }
    if(flip180){ mx=W-1-mx; my=H-1-my; }
    int ox=(fbw-W)/2, oy=(fbh-H)/2; *dx=ox+mx; *dy=oy+my;
}
static inline void put_px_ui(uint8_t *fb,int fbw,int fbh,int xL,int yL,UiOrient o,int flip180,uint8_t r,uint8_t g,uint8_t b,uint8_t a,const Layout *L){
    int dx,dy; map_ui_xy_fb(xL,yL,o,flip180,&dx,&dy,fbw,fbh,L);
    if((unsigned)dx>=(unsigned)fbw||(unsigned)dy>=(unsigned)fbh) return;
    uint8_t p[4]; p[3]=a; p[0]=(uint8_t)((r*a+127)/255); p[1]=(uint8_t)((g*a+127)/255); p[2]=(uint8_t)((b*a+127)/255);
    over_premul(fb+4*(dy*fbw+dx),p);
}
static void draw_overlay_ui(uint8_t *fb,int fbw,int fbh,Overlay ov, UiOrient o,int flip180,const Layout *L){
    int LW=(o==ORIENT_PORTRAIT)?W:H, LH=(o==ORIENT_PORTRAIT)?H:W;
    int x0=ov.x<0?0:ov.x, y0=ov.y<0?0:ov.y, x1=ov.x+ov.w, y1=ov.y+ov.h;
    if(x1>LW)x1=LW;
    if(y1>LH)y1=LH;
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++) put_px_ui(fb,fbw,fbh,x,y,o,flip180,ov.r,ov.g,ov.b,ov.a,L);
}

// TTF cache & draw ---------------------------------------------------------------
typedef struct {
    char path[512]; int px; int valid;
    unsigned char *ttf_data; size_t ttf_size;
    stbtt_fontinfo info; float scale; int ascent,descent,lineGap;
} TtfCache;
static TtfCache g_ttf_cache[4];

static TtfCache* ttf_get(const char *path, int px){
    if(!path||px<=0) return NULL;
    for(int i=0;i<4;i++) if(g_ttf_cache[i].valid && g_ttf_cache[i].px==px && !strcmp(g_ttf_cache[i].path,path)) return &g_ttf_cache[i];
    int slot=-1; for(int i=0;i<4;i++) if(!g_ttf_cache[i].valid){ slot=i; break; } if(slot<0) slot=0;
    FILE *f=fopen(path,"rb"); if(!f){ fprintf(stderr,"ttf open failed: %s\n",path); return NULL; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){ fclose(f); fprintf(stderr,"ttf size invalid: %s\n",path); return NULL; }
    unsigned char *data=(unsigned char*)malloc((size_t)sz); if(!data){ fclose(f); fprintf(stderr,"ttf malloc failed\n"); return NULL; }
    if(fread(data,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); free(data); fprintf(stderr,"ttf read failed\n"); return NULL; } fclose(f);
    stbtt_fontinfo info; if(!stbtt_InitFont(&info,data,stbtt_GetFontOffsetForIndex(data,0))){ free(data); fprintf(stderr,"ttf init failed: %s\n",path); return NULL; }
    float scale=stbtt_ScaleForPixelHeight(&info,(float)px); int a,d,lg; stbtt_GetFontVMetrics(&info,&a,&d,&lg);
    TtfCache *c=&g_ttf_cache[slot]; if(c->valid && c->ttf_data) free(c->ttf_data); memset(c,0,sizeof *c);
    size_t path_len = strlen(path);
    if(path_len >= sizeof(c->path)) path_len = sizeof(c->path) - 1;
    memcpy(c->path, path, path_len);
    c->path[path_len] = '\0';
    c->px=px; c->ttf_data=data; c->ttf_size=(size_t)sz; c->info=info;
    c->scale=scale; c->ascent=a; c->descent=d; c->lineGap=lg; c->valid=1; return c;
}
static const unsigned char* utf8_next(const unsigned char *s, int *cp){
    unsigned c0=s[0]; if(c0<0x80){ *cp=(int)c0; return s+1; }
    if((c0&0xE0)==0xC0){ unsigned c1=s[1]; if((c1&0xC0)!=0x80) goto bad; unsigned v=((c0&0x1F)<<6)|(c1&0x3F); if(v<0x80) goto bad; *cp=(int)v; return s+2; }
    if((c0&0xF0)==0xE0){ unsigned c1=s[1],c2=s[2]; if((c1&0xC0)!=0x80||(c2&0xC0)!=0x80) goto bad; unsigned v=((c0&0x0F)<<12)|((c1&0x3F)<<6)|(c2&0x3F); if(v<0x800) goto bad; *cp=(int)v; return s+3; }
    if((c0&0xF8)==0xF0){ unsigned c1=s[1],c2=s[2],c3=s[3]; if((c1&0xC0)!=0x80||(c2&0xC0)!=0x80||(c3&0xC0)!=0x80) goto bad; unsigned v=((c0&0x07)<<18)|((c1&0x3F)<<12)|((c2&0x3F)<<6)|((c3&0x3F)); if(v<0x10000||v>0x10FFFF) goto bad; *cp=(int)v; return s+4; }
bad: *cp=0xFFFD; return s+1;
}
static void draw_text_ttf(uint8_t *fb,int fbw,int fbh,const TextItem *ti, UiOrient global_o,int global_flip,const Layout *L,const Metrics *M){
    const char *path = ti->ttf_path ? ti->ttf_path : (L->default_ttf[0]? L->default_ttf : NULL);
    int px = ti->ttf_px>0 ? ti->ttf_px : (L->default_ttf_px>0 ? L->default_ttf_px : 0);
    if(!path||px<=0){ fprintf(stderr,"[text] missing TTF/size; skipping \"%s\"\n", ti->text?ti->text:""); return; }
    TtfCache *fc=ttf_get(path,px); if(!fc) return;

    UiOrient o=global_o; int flip=global_flip; int ccw=L->text_landscape_ccw;
    if(ti->orient_override!=-1) o=(ti->orient_override==1)?ORIENT_LANDSCAPE:ORIENT_PORTRAIT;
    if(ti->flip_override!=-1) flip=ti->flip_override;
    if(ti->landscape_ccw_override!=-1) ccw=ti->landscape_ccw_override;
    Layout Lloc=*L; Lloc.text_landscape_ccw=ccw;

    char buf[1024]; expand_tokens(buf,sizeof buf, ti->text?ti->text:"", M);
    float scale=fc->scale; int x=ti->x; int baseline=ti->y + (int)(fc->ascent*scale+0.5f);
    int line_adv=(int)((fc->ascent - fc->descent + fc->lineGap)*scale + 0.5f); int prev=0;

    const unsigned char *p=(const unsigned char*)buf;
    while(*p){
        int cp=0; const unsigned char *np=utf8_next(p,&cp);
        if(cp=='\n'){ x=ti->x; baseline+=line_adv; prev=0; p=np; continue; }
        int ax,lsb; int kern=0; if(prev){ kern=stbtt_GetCodepointKernAdvance(&fc->info,prev,cp); x+=(int)(kern*scale+0.5f); }
        stbtt_GetCodepointHMetrics(&fc->info,cp,&ax,&lsb);
        int x0,y0,x1,y1; stbtt_GetCodepointBitmapBox(&fc->info,cp,scale,scale,&x0,&y0,&x1,&y1);
        int gw=x1-x0, gh=y1-y0;
        if(gw>0 && gh>0){
            unsigned char *bmp=(unsigned char*)malloc((size_t)gw*gh);
            if(bmp){
                stbtt_MakeCodepointBitmap(&fc->info,bmp,gw,gh,gw,scale,scale,cp);
                for(int by=0;by<gh;by++)for(int bx=0;bx<gw;bx++){
                    int a8=bmp[by*gw+bx]; if(!a8) continue; int A=(a8*ti->a+127)/255;
                    put_px_ui(fb,fbw,fbh, x+x0+bx, baseline+y0+by, o,flip, ti->r,ti->g,ti->b,(uint8_t)A,&Lloc);
                }
                free(bmp);
            }
        }
        x+=(int)(ax*scale+0.5f); prev=cp; p=np;
    }
}

// PNG static loader --------------------------------------------------------------
typedef struct { int w,h; uint8_t *rgba; } ImageRGBA;
static ImageRGBA load_png_rgba_stb(const char *path){
    ImageRGBA p={0}; int comp=0; unsigned char *data=stbi_load(path,&p.w,&p.h,&comp,4);
    if(!data){ fprintf(stderr,"stbi_load failed: %s\n", path); exit(1); }
    p.rgba=data; premultiply_rgba(p.rgba,p.w,p.h); return p;
}
static void free_imgrgba(ImageRGBA *p){ if(p->rgba){ stbi_image_free(p->rgba); p->rgba=NULL; } }

// APNG precompose via LodePNG ----------------------------------------------------
typedef struct {
    int is_apng;                // 1 if animated
    unsigned plays;             // 0=infinite (as in file)
    unsigned num_frames;
    unsigned total_ms;          // sum of delays (>=1 per frame)
    unsigned canvas_w, canvas_h;
    uint8_t **frame_rgba;       // num_frames items; size canvas_w*canvas_h*4 each (premultiplied)
    unsigned *delay_ms;         // num_frames items; each >=10ms minimum
} ApngAnim;

static void apnganim_free(ApngAnim *A){
    if(!A) return;
    if(A->frame_rgba){
        for(unsigned i=0;i<A->num_frames;i++) free(A->frame_rgba[i]);
        free(A->frame_rgba);
    }
    free(A->delay_ms);
    memset(A,0,sizeof *A);
}

// CRC32 for PNG chunk
static uint32_t png_crc32(const unsigned char *buf, size_t len){
    static uint32_t table[256]; static int init=0;
    if(!init){
        for(uint32_t n=0;n<256;n++){
            uint32_t c=n;
            for(int k=0;k<8;k++) c = (c&1) ? (0xEDB88320u ^ (c>>1)) : (c>>1);
            table[n]=c;
        }
        init=1;
    }
    uint32_t c=0xffffffffu;
    for(size_t i=0;i<len;i++) c = table[(c ^ buf[i]) & 0xff] ^ (c>>8);
    return c ^ 0xffffffffu;
}
typedef struct { unsigned char *data; size_t size, cap; } ByteVec;
static void bv_init(ByteVec *v){ v->data=NULL; v->size=0; v->cap=0; }
static void bv_reserve(ByteVec *v, size_t need){ if(v->cap>=need) return; size_t nc=v->cap? v->cap*2 : 1024; while(nc<need) nc*=2; v->data=(unsigned char*)realloc(v->data,nc); if(!v->data) die("realloc bv"); v->cap=nc; }
static void bv_push(ByteVec *v, const void *p, size_t n){ bv_reserve(v, v->size+n); memcpy(v->data+v->size,p,n); v->size+=n; }
static void bv_free(ByteVec *v){ free(v->data); v->data=NULL; v->size=v->cap=0; }
static void write_chunk(ByteVec *v, const char type[4], const unsigned char *data, size_t len){
    unsigned char lenb[4]; be32w(lenb,(uint32_t)len); bv_push(v,lenb,4);
    unsigned char typeb[4]={ (unsigned char)type[0], (unsigned char)type[1], (unsigned char)type[2], (unsigned char)type[3] };
    bv_push(v,typeb,4);
    if(len) bv_push(v,data,len);
    // CRC over type+data
    ByteVec tmp; bv_init(&tmp); bv_push(&tmp,typeb,4); if(len) bv_push(&tmp,data,len);
    uint32_t crc=png_crc32(tmp.data,tmp.size); unsigned char crcb[4]; be32w(crcb,crc);
    bv_push(v,crcb,4); bv_free(&tmp);
}

typedef struct { const unsigned char *p; size_t left; } Reader;

typedef struct { unsigned char *data; size_t len; } Chunk;
static int next_chunk(Reader *r, Chunk *ch){
    if(r->left<12) return 0;
    unsigned char lenb[4], typeb[4];
    memcpy(lenb,r->p,4); memcpy(typeb,r->p+4,4);
    uint32_t len=be32r(lenb);
    if(r->left < 12u + len) return 0;
    ch->data = (unsigned char*)(r->p);
    ch->len  = 12u + len; // length + type + data + crc
    r->p += ch->len; r->left -= ch->len;
    return 1;
}
static const unsigned char* chunk_type_ptr(const Chunk *ch){ return ch->data+4; }
static const unsigned char* chunk_dataptr(const Chunk *ch){ return ch->data+8; }
static uint32_t chunk_datalen(const Chunk *ch){ return be32r(ch->data); }

// Precompose APNG into full frames (fixed fcTL offsets; robust tiny-PNG build)
static int apng_load_precompose(const char *path, ApngAnim *A, int rotate180_all){
    memset(A,0,sizeof *A);

    // Load entire file
    unsigned char *filedata=NULL; size_t filesize=0;
    unsigned err = lodepng_load_file(&filedata,&filesize,(const char*)path);
    if(err || !filedata || filesize<33){ fprintf(stderr,"apng: file read failed %s (%u)\n", path, err); free(filedata); return -1; }

    // PNG signature
    static const unsigned char sig[8]={137,80,78,71,13,10,26,10};
    if(memcmp(filedata,sig,8)!=0){ free(filedata); return -1; }

    Reader r={ filedata+8, filesize-8 };

    // Parse IHDR and collect safe header chunks (before first IDAT/fdAT)
    unsigned char ihdr_base[13]={0}; int ihdr_base_set=0;
    Chunk ch; ByteVec header_chunks; bv_init(&header_chunks);
    unsigned acTL_plays=0;
    int saw_acTL=0, saw_IDAT_or_fd=0;

    typedef struct {
        unsigned w,h,x,y, delay_num, delay_den; unsigned char dispose_op, blend_op;
        ByteVec idata; int in_use;
    } FrameBuild;

    FrameBuild cur; memset(&cur,0,sizeof cur); bv_init(&cur.idata);
    unsigned canvas_w=0, canvas_h=0;

    // Composition canvas
    uint8_t *canvas=NULL,*canvas_prev=NULL;

    // Iterate chunks
    while(next_chunk(&r,&ch)){
        const unsigned char *type = chunk_type_ptr(&ch);
        const unsigned char *data = chunk_dataptr(&ch);
        uint32_t dlen = chunk_datalen(&ch);

        if(memcmp(type,"IHDR",4)==0){
            if(dlen!=13){ fprintf(stderr,"apng: bad IHDR\n"); goto fail; }
            memcpy(ihdr_base,data,13); ihdr_base_set=1;
            canvas_w = be32r(data+0); canvas_h = be32r(data+4);
            continue;
        }
        if(!ihdr_base_set){ fprintf(stderr,"apng: IHDR missing\n"); goto fail; }

        if(memcmp(type,"acTL",4)==0){
            if(dlen!=8){ fprintf(stderr,"apng: bad acTL\n"); goto fail; }
            acTL_plays = be32r(data+4);
            saw_acTL=1;
            continue;
        }
        if(memcmp(type,"fcTL",4)==0){
            if(dlen!=26){ fprintf(stderr,"apng: bad fcTL\n"); goto fail; }

            // finalize previous frame (if any)
            if(cur.in_use){
                if(cur.idata.size==0){ // empty frame: ignore safely
                    bv_free(&cur.idata); memset(&cur,0,sizeof cur);
                } else {
                    // Build minimal PNG for the frame IDAT
                    ByteVec png; bv_init(&png); bv_push(&png, sig, 8);

                    unsigned char IHDR_mod[13]; memcpy(IHDR_mod,ihdr_base,13);
                    be32w(IHDR_mod+0, cur.w); be32w(IHDR_mod+4, cur.h);
                    write_chunk(&png,"IHDR",IHDR_mod,13);

                    if(header_chunks.size){
                        // Append raw pre-IDAT ancillary chunks as-is (length+type+data+crc)
                        bv_push(&png, header_chunks.data, header_chunks.size);
                    }

                    write_chunk(&png,"IDAT", cur.idata.data, cur.idata.size);
                    write_chunk(&png,"IEND", NULL, 0);

                    // Decode using lodepng state (tolerant)
                    /*unsigned char *fr=NULL; unsigned fw=0,fh=0;
                    LodePNGState st; lodepng_state_init(&st);
                    st.decoder.ignore_crc = 1;
                    st.decoder.zlibsettings.ignore_nlen = 1;
                    st.decoder.zlibsettings.ignore_adler32 = 1;

                    err = lodepng_decode(&fr,&fw,&fh,&st,png.data,png.size,LCT_RGBA,8);
                    if(err){
                        fprintf(stderr,"apng: decode frame failed %u: %s\n", err, lodepng_error_text(err));
                        lodepng_state_cleanup(&st); bv_free(&png); goto fail;
                    }
                    lodepng_state_cleanup(&st);
                    bv_free(&png);*/

                    unsigned char *fr=NULL; unsigned fw=0,fh=0;
                    int derr = decode_png_rgba_lenient(png.data, png.size, &fr, &fw, &fh);
                    if(derr){ bv_free(&png); goto fail; }
                    bv_free(&png);


                    if(fw!=cur.w || fh!=cur.h){
                        fprintf(stderr,"apng: decoded size mismatch: got %ux%u, expected %ux%u\n", fw,fh,cur.w,cur.h);
                        stbi_image_free(fr); goto fail;
                    }

                    if(!canvas){
                        canvas=(uint8_t*)calloc((size_t)canvas_w*canvas_h,4);
                        canvas_prev=(uint8_t*)calloc((size_t)canvas_w*canvas_h,4);
                        if(!canvas||!canvas_prev) die("calloc apng canvas");
                    }

                    premultiply_rgba(fr, fw, fh);
                    if(rotate180_all){ rotate180_rgba(fr, fw, fh); }

                    if(cur.dispose_op==2){ memcpy(canvas_prev, canvas, (size_t)canvas_w*canvas_h*4); }

                    // Clamp placement safely
                    unsigned maxw = cur.w, maxh = cur.h;
                    if((uint64_t)cur.x + maxw > canvas_w) maxw = canvas_w - cur.x;
                    if((uint64_t)cur.y + maxh > canvas_h) maxh = canvas_h - cur.y;

                    // Blend
                    for(unsigned y=0;y<maxh;y++){
                        int dy=cur.y+(int)y;
                        for(unsigned x=0;x<maxw;x++){
                            int dx=cur.x+(int)x;
                            uint8_t *dst = canvas + 4*((size_t)dy*canvas_w + dx);
                            const uint8_t *src = fr + 4*((size_t)y*cur.w + x);
                            if(cur.blend_op==0){ // SOURCE
                                dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; dst[3]=src[3];
                            }else{ // OVER
                                over_premul(dst, src);
                            }
                        }
                    }

                    // Store display frame
                    A->frame_rgba = (uint8_t**)realloc(A->frame_rgba, (A->num_frames+1)*sizeof(uint8_t*));
                    A->delay_ms   = (unsigned*)realloc(A->delay_ms,   (A->num_frames+1)*sizeof(unsigned));
                    if(!A->frame_rgba||!A->delay_ms) die("realloc apng frames");
                    uint8_t *frame_copy=(uint8_t*)malloc((size_t)canvas_w*canvas_h*4);
                    if(!frame_copy) die("malloc apng frame");
                    memcpy(frame_copy, canvas, (size_t)canvas_w*canvas_h*4);
                    A->frame_rgba[A->num_frames]=frame_copy;

                    unsigned den = cur.delay_den? cur.delay_den : 100; // 0 => 100 (centiseconds)
                    unsigned num = cur.delay_num? cur.delay_num : 1;   // clamp minimum
                    unsigned ms = (unsigned)((1000ull*num + den/2) / den);
                    if(ms<10) ms=10;
                    A->delay_ms[A->num_frames]=ms;
                    A->total_ms += ms;
                    A->num_frames++;

                    // Dispose
                    if(cur.dispose_op==1){ // BACKGROUND
                        for(unsigned y=0;y<maxh;y++){
                            int dy=cur.y+(int)y;
                            for(unsigned x=0;x<maxw;x++){
                                int dx=cur.x+(int)x;
                                uint8_t *dst = canvas + 4*((size_t)dy*canvas_w + dx);
                                dst[0]=dst[1]=dst[2]=dst[3]=0;
                            }
                        }
                    } else if(cur.dispose_op==2){ // PREVIOUS
                        memcpy(canvas, canvas_prev, (size_t)canvas_w*canvas_h*4);
                    }

                    stbi_image_free(fr);
                    bv_free(&cur.idata); memset(&cur,0,sizeof cur);
                }
            }

            // Start new frame (skip seq number: 4 bytes)
            cur.w = be32r(data + 4);
            cur.h = be32r(data + 8);
            cur.x = be32r(data + 12);
            cur.y = be32r(data + 16);
            cur.delay_num = (data[20]<<8) | data[21];
            cur.delay_den = (data[22]<<8) | data[23];
            cur.dispose_op = data[24];
            cur.blend_op   = data[25];

            if(cur.w==0 || cur.h==0){ fprintf(stderr,"apng: bad fcTL (zero size)\n"); goto fail; }
            bv_init(&cur.idata); cur.in_use=1;
            continue;
        }
        if(memcmp(type,"fdAT",4)==0){
            if(!cur.in_use) continue; // stray
            if(dlen<4) { fprintf(stderr,"apng: bad fdAT\n"); goto fail; }
            bv_push(&cur.idata, data+4, dlen-4); // skip seq
            saw_IDAT_or_fd=1;
            continue;
        }
        if(memcmp(type,"IDAT",4)==0){
            if(!cur.in_use){
                // Frame 0 without prior fcTL: synthesize default
                cur.w = canvas_w; cur.h = canvas_h; cur.x=0; cur.y=0;
                cur.delay_num=10; cur.delay_den=100; cur.dispose_op=0; cur.blend_op=0;
                bv_init(&cur.idata); cur.in_use=1;
            }
            bv_push(&cur.idata, data, dlen);
            saw_IDAT_or_fd=1;
            continue;
        }
        if(memcmp(type,"IEND",4)==0){
            // finalize any pending frame
            if(cur.in_use && cur.idata.size>0){
                ByteVec png; bv_init(&png); bv_push(&png,sig,8);
                unsigned char IHDR_mod[13]; memcpy(IHDR_mod,ihdr_base,13);
                be32w(IHDR_mod+0, cur.w); be32w(IHDR_mod+4, cur.h);
                write_chunk(&png,"IHDR",IHDR_mod,13);
                if(header_chunks.size){ bv_push(&png, header_chunks.data, header_chunks.size); }
                write_chunk(&png,"IDAT", cur.idata.data, cur.idata.size);
                write_chunk(&png,"IEND", NULL, 0);

                unsigned char *fr = NULL; unsigned fw = 0, fh = 0;
                int derr = decode_png_rgba_lenient(png.data, png.size, &fr, &fw, &fh);
                if (derr) { bv_free(&png); goto fail; }
                bv_free(&png);

                if(fw!=cur.w || fh!=cur.h){
                    fprintf(stderr,"apng: decoded size mismatch: got %ux%u, expected %ux%u\n", fw,fh,cur.w,cur.h);
                    stbi_image_free(fr); goto fail;
                }

                if(!canvas){
                    canvas=(uint8_t*)calloc((size_t)canvas_w*canvas_h,4);
                    canvas_prev=(uint8_t*)calloc((size_t)canvas_w*canvas_h,4);
                    if(!canvas||!canvas_prev) die("calloc apng canvas");
                }

                premultiply_rgba(fr, fw, fh);
                if(rotate180_all){ rotate180_rgba(fr, fw, fh); }
                if(cur.dispose_op==2){ memcpy(canvas_prev,canvas,(size_t)canvas_w*canvas_h*4); }

                unsigned maxw = cur.w, maxh = cur.h;
                if((uint64_t)cur.x + maxw > canvas_w) maxw = canvas_w - cur.x;
                if((uint64_t)cur.y + maxh > canvas_h) maxh = canvas_h - cur.y;

                for(unsigned y=0;y<maxh;y++){
                    int dy=cur.y+(int)y;
                    for(unsigned x=0;x<maxw;x++){
                        int dx=cur.x+(int)x;
                        uint8_t *dst = canvas + 4*((size_t)dy*canvas_w + dx);
                        const uint8_t *src = fr + 4*((size_t)y*cur.w + x);
                        if(cur.blend_op==0){ dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; dst[3]=src[3]; }
                        else over_premul(dst,src);
                    }
                }
                A->frame_rgba = (uint8_t**)realloc(A->frame_rgba, (A->num_frames+1)*sizeof(uint8_t*));
                A->delay_ms   = (unsigned*)realloc(A->delay_ms,   (A->num_frames+1)*sizeof(unsigned));
                if(!A->frame_rgba||!A->delay_ms) die("realloc apng frames");
                uint8_t *frame_copy=(uint8_t*)malloc((size_t)canvas_w*canvas_h*4); if(!frame_copy) die("malloc apng frame");
                memcpy(frame_copy, canvas, (size_t)canvas_w*canvas_h*4);
                A->frame_rgba[A->num_frames]=frame_copy;

                unsigned den = cur.delay_den? cur.delay_den : 100;
                unsigned num = cur.delay_num? cur.delay_num : 1;
                unsigned ms = (unsigned)((1000ull*num + den/2) / den); if(ms<10) ms=10;
                A->delay_ms[A->num_frames]=ms;
                A->total_ms += ms; A->num_frames++;

                if(cur.dispose_op==1){
                    for(unsigned y=0;y<maxh;y++){ int dy=cur.y+(int)y;
                        for(unsigned x=0;x<maxw;x++){ int dx=cur.x+(int)x;
                            uint8_t *dst=canvas+4*((size_t)dy*canvas_w+dx); dst[0]=dst[1]=dst[2]=dst[3]=0; } }
                } else if(cur.dispose_op==2){ memcpy(canvas,canvas_prev,(size_t)canvas_w*canvas_h*4); }
                stbi_image_free(fr);
                bv_free(&cur.idata); memset(&cur,0,sizeof cur);
            }
            break;
        }

        // Header chunk collection (before first IDAT/fdAT only; skip acTL/fcTL)
        if(!saw_IDAT_or_fd){
            if(memcmp(type,"acTL",4)!=0 && memcmp(type,"fcTL",4)!=0 && memcmp(type,"IHDR",4)!=0){
                // copy entire chunk raw (length+type+data+crc)
                bv_push(&header_chunks, ch.data, ch.len);
            }
        }
    }

    bv_free(&header_chunks);
    free(filedata);

    if(!saw_acTL || A->num_frames==0){
        // Not animated: let caller decode as static
        apnganim_free(A);
        return 1; // signal: static PNG
    }

    A->is_apng=1; A->plays=acTL_plays; A->canvas_w=canvas_w; A->canvas_h=canvas_h;
    free(canvas); free(canvas_prev);
    return 0;

fail:
    bv_free(&header_chunks);
    free(filedata);
    apnganim_free(A);
    return -1;
}

// Choose frame by time/loops/speed ----------------------------------------------
static unsigned apng_pick_frame(const ApngAnim *A, uint64_t base_ms, double speed,
                                int loop_mode, int loop_N, unsigned *out_remaining_ms){
    if(A->num_frames==0){ if(out_remaining_ms) *out_remaining_ms=0; return 0; }
    if(A->total_ms==0){ if(out_remaining_ms) *out_remaining_ms=0; return A->num_frames-1; }
    uint64_t t = (uint64_t)((double)base_ms * (speed>0?speed:1.0));
    unsigned loops_file = (A->plays==0)? UINT32_MAX : A->plays;
    unsigned loops = loops_file;
    if(loop_mode==1) loops=UINT32_MAX; // infinite
    else if(loop_mode==2) loops=1;     // once
    else if(loop_mode==3) loops=(loop_N==0?1:(unsigned)loop_N);

    uint64_t duration = A->total_ms;
    uint64_t cycle = t / duration;
    if(cycle >= loops){
        if(out_remaining_ms) *out_remaining_ms=0;
        return A->num_frames-1;
    }
    uint64_t in_cycle = t % duration;
    unsigned idx=0; unsigned acc=0;
    for(; idx<A->num_frames; idx++){
        unsigned d=A->delay_ms[idx];
        if(in_cycle < acc + d) break;
        acc += d;
    }
    if(idx>=A->num_frames) idx=A->num_frames-1;
    if(out_remaining_ms) *out_remaining_ms = (unsigned)((acc + A->delay_ms[idx]) - in_cycle);
    return idx;
}

// Video decoding via FFmpeg -----------------------------------------------------
typedef struct {
    AVFormatContext *format_ctx;
    AVCodecContext *codec_ctx;
    const AVCodec *codec;
    struct SwsContext *sws_ctx;
    AVFrame *frame;
    AVFrame *frame_rgb;
    AVPacket *packet;
    int video_stream_idx;
    int64_t duration_ms;
    int64_t start_time;
    int width, height;        // Original video dimensions
    int target_width, target_height; // Target LCD dimensions (320x240)
    uint8_t *rgb_buffer;      // RGB565 buffer from FFmpeg (scaled, not letterboxed)
    uint8_t *letterbox_buffer; // RGB565 buffer for letterboxed result (320x320)
    uint8_t *rgba_buffer;     // Temporary RGBA buffer for blitting
    uint8_t *rotated_buffer;  // Buffer for rotated frame
    int rotation_angle;       // 0, 90, 180, 270 degrees
    int loaded;
    int64_t current_pts;      // Current presentation timestamp
    int64_t last_seek_pts;    // Last seek position
    // Optimization: frame caching
    uint8_t *cached_frame;    // Cached frame data
    int64_t cached_pts;       // PTS of cached frame
    int cache_valid;          // Is cache valid?
    // Optimization: reduce seeking
    int64_t last_request_pts; // Last requested PTS
    int frame_skip_count;     // Count of frames to skip
    // Video frame rate detection
    float video_fps;          // Native video frame rate
    int fps_detected;         // Whether FPS was successfully detected
    
    // Full video caching for short videos
    uint8_t **cached_frames;  // Array of cached frame data
    int *cached_timestamps;   // Array of frame timestamps in ms
    int num_cached_frames;    // Number of frames in cache
    int cache_enabled;        // Whether caching is enabled
    int cache_loaded;         // Whether cache has been loaded
} VideoDecoder;

static void video_decoder_free(VideoDecoder *vd){
    if(!vd) return;
    if(vd->sws_ctx) sws_freeContext(vd->sws_ctx);
    if(vd->frame) av_frame_free(&vd->frame);
    if(vd->frame_rgb) av_frame_free(&vd->frame_rgb);
    if(vd->packet) av_packet_free(&vd->packet);
    if(vd->codec_ctx) avcodec_free_context(&vd->codec_ctx);
    if(vd->format_ctx) avformat_close_input(&vd->format_ctx);
    if(vd->rgb_buffer) free(vd->rgb_buffer);
    if(vd->letterbox_buffer) free(vd->letterbox_buffer);
    if(vd->rgba_buffer) free(vd->rgba_buffer);
    if(vd->rotated_buffer) free(vd->rotated_buffer);
    if(vd->cached_frame) free(vd->cached_frame);
    
    // Free video cache
    if(vd->cached_frames && vd->num_cached_frames > 0){
        for(int i = 0; i < vd->num_cached_frames; i++){
            if(vd->cached_frames[i]) free(vd->cached_frames[i]);
        }
        free(vd->cached_frames);
    }
    if(vd->cached_timestamps) free(vd->cached_timestamps);
    
    memset(vd,0,sizeof *vd);
}

static int video_decoder_load(const char *path, VideoDecoder *vd, int rotation_angle){
    memset(vd,0,sizeof *vd);
    
    // Initialize FFmpeg (av_register_all is deprecated in newer versions)
    // av_register_all(); // Not needed in FFmpeg 4.0+
    avformat_network_init();
    
    // Open input file
    if(avformat_open_input(&vd->format_ctx, path, NULL, NULL) < 0){
        fprintf(stderr,"video: could not open file %s\n", path);
        return -1;
    }
    
    // Find stream information
    if(avformat_find_stream_info(vd->format_ctx, NULL) < 0){
        fprintf(stderr,"video: could not find stream info\n");
        video_decoder_free(vd);
        return -1;
    }
    
    // Find video stream
    vd->video_stream_idx = -1;
    for(unsigned i = 0; i < vd->format_ctx->nb_streams; i++){
        if(vd->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
            vd->video_stream_idx = i;
            break;
        }
    }
    
    if(vd->video_stream_idx == -1){
        fprintf(stderr,"video: no video stream found\n");
        video_decoder_free(vd);
        return -1;
    }
    
    // Get codec parameters
    AVCodecParameters *codecpar = vd->format_ctx->streams[vd->video_stream_idx]->codecpar;
    
    // Find decoder
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if(!codec){
        fprintf(stderr,"video: unsupported codec\n");
        video_decoder_free(vd);
        return -1;
    }
    
    // Allocate codec context
    vd->codec_ctx = avcodec_alloc_context3(codec);
    if(!vd->codec_ctx){
        fprintf(stderr,"video: could not allocate codec context\n");
        video_decoder_free(vd);
        return -1;
    }
    
    // Copy codec parameters to context
    if(avcodec_parameters_to_context(vd->codec_ctx, codecpar) < 0){
        fprintf(stderr,"video: could not copy codec parameters\n");
        video_decoder_free(vd);
        return -1;
    }
    
    // Open codec
    if(avcodec_open2(vd->codec_ctx, codec, NULL) < 0){
        fprintf(stderr,"video: could not open codec\n");
        video_decoder_free(vd);
        return -1;
    }
    
    // Allocate frames
    vd->frame = av_frame_alloc();
    vd->frame_rgb = av_frame_alloc();
    vd->packet = av_packet_alloc();
    
    if(!vd->frame || !vd->frame_rgb || !vd->packet){
        fprintf(stderr,"video: could not allocate frames/packet\n");
        video_decoder_free(vd);
        return -1;
    }
    
    // Set target to 320x320 square for proper framebuffer fit
    vd->target_width = 320;   // Square width (320)
    vd->target_height = 320;  // Square height (320)
    
    // Calculate scaling to preserve aspect ratio
    int src_w = vd->codec_ctx->width;
    int src_h = vd->codec_ctx->height;
    
    // Calculate scale factor to fit within 320x320 while preserving aspect ratio
    float scale_w = 320.0f / src_w;
    float scale_h = 320.0f / src_h;
    float scale = (scale_w < scale_h) ? scale_w : scale_h; // Use smaller scale to fit
    
    // Set dimensions to scaled size (not necessarily 320x320)
    vd->width = (int)(src_w * scale);
    vd->height = (int)(src_h * scale);
    
    
    vd->sws_ctx = sws_getContext(src_w, src_h, vd->codec_ctx->pix_fmt,
                                 vd->width, vd->height, AV_PIX_FMT_RGB565LE,
                                 SWS_LANCZOS, NULL, NULL, NULL);
    
    if(!vd->sws_ctx){
        fprintf(stderr,"video: could not create scaling context\n");
        video_decoder_free(vd);
        return -1;
    }
    
    // Allocate RGB565 buffer for scaled video
    int rgb_size = av_image_get_buffer_size(AV_PIX_FMT_RGB565LE, vd->width, vd->height, 1);
    vd->rgb_buffer = (uint8_t*)malloc(rgb_size);
    if(!vd->rgb_buffer){
        fprintf(stderr,"video: could not allocate RGB565 buffer\n");
        video_decoder_free(vd);
        return -1;
    }
    
    av_image_fill_arrays(vd->frame_rgb->data, vd->frame_rgb->linesize,
                         vd->rgb_buffer, AV_PIX_FMT_RGB565LE, vd->width, vd->height, 1);
    
    // Allocate letterbox buffer for 320x320 result
    int letterbox_size = av_image_get_buffer_size(AV_PIX_FMT_RGB565LE, 320, 320, 1);
    vd->letterbox_buffer = (uint8_t*)malloc(letterbox_size);
    if(!vd->letterbox_buffer){
        fprintf(stderr,"video: could not allocate letterbox buffer\n");
        video_decoder_free(vd);
        return -1;
    }
    
    // Allocate temporary RGBA buffer for framebuffer blitting
    int rgba_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, 320, 320, 1);
    vd->rgba_buffer = (uint8_t*)malloc(rgba_size);
    if(!vd->rgba_buffer){
        fprintf(stderr,"video: could not allocate RGBA buffer\n");
        video_decoder_free(vd);
        return -1;
    }
    
    // Set up rotation to compensate for LCD hardware rotation
    vd->rotation_angle = rotation_angle;
    if(rotation_angle == 90 || rotation_angle == 270){
        // For 90/270 degree rotation, dimensions are swapped
        int rotated_size = av_image_get_buffer_size(AV_PIX_FMT_RGB565LE, vd->height, vd->width, 1);
        vd->rotated_buffer = (uint8_t*)malloc(rotated_size);
        if(!vd->rotated_buffer){
            fprintf(stderr,"video: could not allocate rotated RGB565 buffer\n");
            video_decoder_free(vd);
            return -1;
        }
    }
    
    // Get duration
    if(vd->format_ctx->duration != AV_NOPTS_VALUE){
        vd->duration_ms = vd->format_ctx->duration / 1000;
    } else {
        vd->duration_ms = 0; // Unknown duration
    }
    
    vd->start_time = vd->format_ctx->streams[vd->video_stream_idx]->start_time;
    if(vd->start_time == AV_NOPTS_VALUE) vd->start_time = 0;
    
    vd->current_pts = vd->start_time;
    vd->last_seek_pts = vd->start_time;
    vd->last_request_pts = vd->start_time;
    vd->frame_skip_count = 0;
    vd->cache_valid = 0;
    vd->cached_pts = 0;
    
    // Detect video frame rate
    AVRational frame_rate = vd->format_ctx->streams[vd->video_stream_idx]->r_frame_rate;
    if(frame_rate.num > 0 && frame_rate.den > 0){
        vd->video_fps = (float)frame_rate.num / (float)frame_rate.den;
        vd->fps_detected = 1;
    } else {
        vd->video_fps = 24.0f; // Default fallback
        vd->fps_detected = 0;
    }
    
    // Allocate cache buffer for final LCD dimensions
    int cache_size = av_image_get_buffer_size(AV_PIX_FMT_RGB565LE, vd->target_width, vd->target_height, 1);
    vd->cached_frame = (uint8_t*)malloc(cache_size);
    if(!vd->cached_frame) {
        fprintf(stderr, "Failed to allocate video cache RGB565 buffer\n");
        video_decoder_free(vd);
        return -1;
    }
    
    vd->loaded = 1;
    
    // Enable caching for videos under 8 seconds (if enabled in config) - reduced to save memory
    if(vd->duration_ms > 0 && vd->duration_ms < 8000){
        vd->cache_enabled = 1; // Will be overridden by config if needed
        fprintf(stderr,"[VIDEO] Caching available for short video (%lldms)\n", (long long)vd->duration_ms);
    } else {
        vd->cache_enabled = 0;
    }
    
    fprintf(stderr,"[VIDEO] loaded: %dx%d, duration=%lldms, fps=%.1f (%s)\n",
            vd->width, vd->height, (long long)vd->duration_ms, vd->video_fps, path);
    
    return 0;
}


// Convert RGB565 to RGBA for framebuffer blitting
static void rgb565_to_rgba(const uint8_t *rgb565, uint8_t *rgba, int width, int height) {
    for(int i = 0; i < width * height; i++) {
        uint16_t pixel = ((uint16_t*)rgb565)[i];
        
        // Extract RGB565 components
        uint8_t r = ((pixel & 0xF800) >> 8) | ((pixel & 0xE000) >> 13); // 5 bits -> 8 bits
        uint8_t g = ((pixel & 0x07E0) >> 3) | ((pixel & 0x0600) >> 9);  // 6 bits -> 8 bits  
        uint8_t b = ((pixel & 0x001F) << 3) | ((pixel & 0x001C) >> 2);  // 5 bits -> 8 bits
        
        rgba[i*4 + 0] = r;
        rgba[i*4 + 1] = g;
        rgba[i*4 + 2] = b;
        rgba[i*4 + 3] = 255; // Alpha
    }
}

// Scale video with letterboxing to fit 320x320 square
static void scale_with_letterbox(const uint8_t *src, uint8_t *dst, int src_w, int src_h) {
    // Calculate scale factor to fit within 320x320 while preserving aspect ratio
    float scale_w = 320.0f / src_w;
    float scale_h = 320.0f / src_h;
    float scale = (scale_w < scale_h) ? scale_w : scale_h; // Use smaller scale to fit
    
    int scaled_w = (int)(src_w * scale);
    int scaled_h = (int)(src_h * scale);
    
    // Calculate letterbox offsets to center the scaled video
    int offset_x = (320 - scaled_w) / 2;
    int offset_y = (320 - scaled_h) / 2;
    
    // Clear destination to black (letterboxing)
    memset(dst, 0, 320 * 320 * 2); // RGB565 is 2 bytes per pixel
    
    // Scale and copy the video content
    for(int y = 0; y < scaled_h; y++) {
        for(int x = 0; x < scaled_w; x++) {
            // Source coordinates
            int src_x = (int)(x / scale);
            int src_y = (int)(y / scale);
            
            // Clamp to source bounds
            if(src_x >= src_w) src_x = src_w - 1;
            if(src_y >= src_h) src_y = src_h - 1;
            
            // Destination coordinates (centered)
            int dst_x = offset_x + x;
            int dst_y = offset_y + y;
            
            // Copy RGB565 pixel (2 bytes)
            int src_idx = (src_y * src_w + src_x) * 2;
            int dst_idx = (dst_y * 320 + dst_x) * 2;
            
            dst[dst_idx + 0] = src[src_idx + 0]; // Low byte
            dst[dst_idx + 1] = src[src_idx + 1]; // High byte
        }
    }
}

// Rotate square video without letterboxing (320x320 to 320x320) - RGB565 format
static void rotate_square(uint8_t *src, uint8_t *dst, int size, int rotation) {
    if(rotation == 0) {
        // No rotation - direct copy
        int frame_size = size * size * 2; // RGB565 is 2 bytes per pixel
        memcpy(dst, src, frame_size);
    } else if(rotation == 90) {
        // 90° clockwise rotation: (x,y) -> (size-1-y, x)
        for(int y = 0; y < size; y++) {
            for(int x = 0; x < size; x++) {
                int src_idx = (y * size + x) * 2; // RGB565 is 2 bytes per pixel
                int dst_x = size - 1 - y;
                int dst_y = x;
                int dst_idx = (dst_y * size + dst_x) * 2;
                
                // Copy RGB565 pixel (2 bytes)
                dst[dst_idx + 0] = src[src_idx + 0]; // Low byte
                dst[dst_idx + 1] = src[src_idx + 1]; // High byte
            }
        }
    } else if(rotation == -90) {
        // -90° (270° clockwise) rotation: (x,y) -> (y, size-1-x)
        for(int y = 0; y < size; y++) {
            for(int x = 0; x < size; x++) {
                int src_idx = (y * size + x) * 2; // RGB565 is 2 bytes per pixel
                int dst_x = y;
                int dst_y = size - 1 - x;
                int dst_idx = (dst_y * size + dst_x) * 2;
                
                // Copy RGB565 pixel (2 bytes)
                dst[dst_idx + 0] = src[src_idx + 0]; // Low byte
                dst[dst_idx + 1] = src[src_idx + 1]; // High byte
            }
        }
    }
}

// Load entire video into cache for short videos
static int video_decoder_load_cache(VideoDecoder *vd){
    if(!vd || !vd->cache_enabled || vd->cache_loaded) return 0;
    
    // Calculate number of frames to cache with frame skipping for performance
    int total_frames = (int)((vd->duration_ms * vd->video_fps) / 1000.0f) + 1;
    
    // Skip frames for very long videos to reduce memory usage
    int frame_skip = 1;
    if(total_frames > 200) frame_skip = 2;  // Skip every other frame for >200 frames
    if(total_frames > 400) frame_skip = 3;  // Skip 2/3 frames for >400 frames
    
    int cached_frames = (total_frames + frame_skip - 1) / frame_skip; // Ceiling division
    int frame_size = av_image_get_buffer_size(AV_PIX_FMT_RGB565LE, vd->target_width, vd->target_height, 1);
    
    fprintf(stderr,"[VIDEO] Loading %d frames into cache (%d MB total, skip=%d) - scaled from %dx%d to %dx%d\n", 
            cached_frames, (cached_frames * frame_size) / (1024*1024), frame_skip,
            vd->codec_ctx->width, vd->codec_ctx->height, vd->target_width, vd->target_height);
    
    // Allocate cache arrays
    vd->cached_frames = (uint8_t**)calloc(cached_frames, sizeof(uint8_t*));
    vd->cached_timestamps = (int*)calloc(cached_frames, sizeof(int));
    if(!vd->cached_frames || !vd->cached_timestamps){
        fprintf(stderr,"[VIDEO] Failed to allocate cache arrays\n");
        return -1;
    }
    
    // Seek to beginning
    if(av_seek_frame(vd->format_ctx, vd->video_stream_idx, vd->start_time, AVSEEK_FLAG_BACKWARD) < 0){
        fprintf(stderr,"[VIDEO] Failed to seek to beginning for caching\n");
        return -1;
    }
    
    avcodec_flush_buffers(vd->codec_ctx);
    
    int frame_count = 0;
    int64_t current_time = 0;
    
    // Decode frames with skipping
    int frames_decoded = 0;
    while(av_read_frame(vd->format_ctx, vd->packet) >= 0 && frame_count < cached_frames){
        if(vd->packet->stream_index == vd->video_stream_idx){
            int ret = avcodec_send_packet(vd->codec_ctx, vd->packet);
            if(ret < 0) {
                av_packet_unref(vd->packet);
                continue;
            }
            
            while(ret >= 0 && frame_count < cached_frames){
                ret = avcodec_receive_frame(vd->codec_ctx, vd->frame);
                if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if(ret < 0) break;
                
                // Skip frames based on frame_skip
                if(frames_decoded % frame_skip != 0) {
                    frames_decoded++;
                    continue;
                }
                
                // Convert to RGBA (scaled to fit LCD)
                sws_scale(vd->sws_ctx, (const uint8_t* const*)vd->frame->data, vd->frame->linesize,
                         0, vd->frame->height, vd->frame_rgb->data, vd->frame_rgb->linesize);
                
                // Allocate frame buffer for final LCD dimensions (RGB565) - reuse if possible
                if(!vd->cached_frames[frame_count]) {
                    vd->cached_frames[frame_count] = (uint8_t*)malloc(frame_size);
                    if(!vd->cached_frames[frame_count]){
                        fprintf(stderr,"[VIDEO] Failed to allocate RGB565 frame %d\n", frame_count);
                        break;
                    }
                }
                
                // Apply letterboxing to make 320x320 square
                scale_with_letterbox(vd->frame_rgb->data[0], vd->letterbox_buffer, vd->width, vd->height);
                
                // Rotate 320x320 square (RGB565)
                rotate_square(vd->letterbox_buffer, vd->cached_frames[frame_count], 320, vd->rotation_angle);
                vd->cached_timestamps[frame_count] = (int)current_time;
                
                frame_count++;
                frames_decoded++;
                current_time += (int64_t)(1000.0f / vd->video_fps); // Convert to milliseconds
            }
        }
        av_packet_unref(vd->packet);
    }
    
    vd->num_cached_frames = frame_count;
    vd->cache_loaded = 1;
    
    fprintf(stderr,"[VIDEO] Cache loaded: %d frames (%d MB)\n", 
            frame_count, (frame_count * frame_size) / (1024*1024));
    
    return 0;
}

static int video_decoder_get_frame(VideoDecoder *vd, uint64_t time_ms, uint8_t **out_rgba, int *out_w, int *out_h){
    if(!vd || !vd->loaded) return -1;
    
    // Handle looping by wrapping time_ms
    if(vd->duration_ms > 0 && time_ms >= (uint64_t)vd->duration_ms){
        time_ms = time_ms % (uint64_t)vd->duration_ms;
    }
    
    // Load cache if enabled and not yet loaded
    if(vd->cache_enabled && !vd->cache_loaded){
        video_decoder_load_cache(vd);
    }
    
    // Use cached frames if available
    if(vd->cache_loaded && vd->num_cached_frames > 0){
        // Find the closest frame in cache
        int best_frame = 0;
        int best_diff = abs((int)time_ms - vd->cached_timestamps[0]);
        
        for(int i = 1; i < vd->num_cached_frames; i++){
            int diff = abs((int)time_ms - vd->cached_timestamps[i]);
            if(diff < best_diff){
                best_diff = diff;
                best_frame = i;
            }
        }
        
        // Convert cached RGB565 frame to RGBA for blitting
        rgb565_to_rgba(vd->cached_frames[best_frame], vd->rgba_buffer, vd->target_width, vd->target_height);
        
        *out_rgba = vd->rgba_buffer;
        *out_w = vd->target_width;   // Always 320x320 square
        *out_h = vd->target_height;  // Always 320x320 square
        return 0;
    }
    
    // Fallback to original decoding for non-cached videos
    int64_t target_pts = vd->start_time + (int64_t)(time_ms * AV_TIME_BASE / 1000);
    
    // Check cache first - if we have a cached frame close to target time, use it
    if(vd->cache_valid){
        int64_t cached_time_ms = (vd->cached_pts - vd->start_time) * 1000 / AV_TIME_BASE;
        int64_t cache_diff = (int64_t)time_ms - cached_time_ms;
        if(cache_diff < 0) cache_diff = -cache_diff;
        
        if(cache_diff < 100){ // Use cached frame if within 100ms
            // Convert cached RGB565 frame to RGBA for blitting
            rgb565_to_rgba(vd->cached_frame, vd->rgba_buffer, vd->target_width, vd->target_height);
            
            *out_rgba = vd->rgba_buffer;
            *out_w = vd->target_width;   // Always 320x320 square
            *out_h = vd->target_height;  // Always 320x320 square
            return 0;
        }
    }
    
    // Check if we need to seek (only if we're far from current position)
    int64_t pts_diff = target_pts - vd->current_pts;
    if(pts_diff < 0) pts_diff = -pts_diff;
    
    // Only seek if we're more than 2 seconds away or at start
    if(pts_diff > 2 * AV_TIME_BASE || vd->current_pts == vd->start_time){
        // Seek to target position
        if(av_seek_frame(vd->format_ctx, vd->video_stream_idx, target_pts, AVSEEK_FLAG_BACKWARD) < 0){
            // If seek fails, start from beginning
            av_seek_frame(vd->format_ctx, vd->video_stream_idx, vd->start_time, AVSEEK_FLAG_BACKWARD);
            vd->current_pts = vd->start_time;
        } else {
            vd->current_pts = target_pts;
        }
        vd->last_seek_pts = vd->current_pts;
        vd->cache_valid = 0; // Invalidate cache after seek
        
        // Flush codec buffers after seek
        avcodec_flush_buffers(vd->codec_ctx);
    }
    
    // Read and decode frames until we get one that's close to our target time
    int frame_count = 0;
    while(av_read_frame(vd->format_ctx, vd->packet) >= 0 && frame_count < 5){
        if(vd->packet->stream_index == vd->video_stream_idx){
            int ret = avcodec_send_packet(vd->codec_ctx, vd->packet);
            if(ret < 0) {
                av_packet_unref(vd->packet);
                continue;
            }
            
            ret = avcodec_receive_frame(vd->codec_ctx, vd->frame);
            if(ret == 0){
                frame_count++;
                // Update current PTS
                if(vd->frame->pts != AV_NOPTS_VALUE){
                    vd->current_pts = vd->frame->pts;
                }
                
                // Check if this frame is close enough to our target time
                int64_t frame_time_ms = (vd->current_pts - vd->start_time) * 1000 / AV_TIME_BASE;
                int64_t time_diff = (int64_t)time_ms - frame_time_ms;
                if(time_diff < 0) time_diff = -time_diff;
                
                // If frame is close enough (within 150ms) or we're at the end, use it
                if(time_diff < 150 || frame_time_ms >= (int64_t)time_ms || frame_count >= 3){
                    // Convert to RGBA (scaled to fit LCD)
                    sws_scale(vd->sws_ctx, (const uint8_t* const*)vd->frame->data, vd->frame->linesize,
                              0, vd->frame->height, vd->frame_rgb->data, vd->frame_rgb->linesize);
                    
                    av_packet_unref(vd->packet);
                    
                    
                    // Apply letterboxing to make 320x320 square
                    scale_with_letterbox(vd->frame_rgb->data[0], vd->letterbox_buffer, vd->width, vd->height);
                    
                    // Rotate 320x320 square (RGB565)
                    rotate_square(vd->letterbox_buffer, vd->rgb_buffer, 320, vd->rotation_angle);
                    
                    // Convert RGB565 to RGBA for framebuffer blitting
                    rgb565_to_rgba(vd->rgb_buffer, vd->rgba_buffer, vd->target_width, vd->target_height);
                    
                    // Cache this frame for future use
                    memcpy(vd->cached_frame, vd->rgb_buffer, av_image_get_buffer_size(AV_PIX_FMT_RGB565LE, vd->target_width, vd->target_height, 1));
                    vd->cached_pts = vd->current_pts;
                    vd->cache_valid = 1;
                    
                    *out_rgba = vd->rgba_buffer;
                    *out_w = vd->target_width;   // Always 320x320 square
                    *out_h = vd->target_height;  // Always 320x320 square
                    return 0;
                }
            }
        }
        av_packet_unref(vd->packet);
    }
    
    return -1;
}

// FB/Viewport & RGB565 -----------------------------------------------------------
static int FBW= 400, FBH= 400;
static void compute_fb(void){ 
    // Make framebuffer larger than video to allow proper centering
    // Video is 320x320, viewport extracts 240x320, so framebuffer should be larger
    FBW = 400; 
    FBH = 400; 
}
static void compute_viewport(const Layout *L, int *vx, int *vy){
    int x=(L->viewport_x<0)?(FBW - W)/2 : L->viewport_x;
    int y=(L->viewport_y<0)?(FBH - H)/2 - 5 : L->viewport_y; // Adjust -5 lines for proper centering
    if(x<0)x=0;
    if(y<0)y=0;
    if(x>FBW-W)x=FBW-W;
    if(y>FBH-H)y=FBH-H;
    *vx=x; *vy=y;
}
static uint8_t* viewport_to_rgb565(const uint8_t *fb,int fbw,int fbh,int vx,int vy){
    (void)fbh; 
    // Extract 240x320 viewport from 400x400 framebuffer for LCD
    size_t px=(size_t)W*H; uint8_t *out=(uint8_t*)malloc(px*2); if(!out) die("malloc565");
    size_t j=0;
    for(int y=0;y<H;y++){
        const uint8_t *row = fb + 4*((vy+y)*fbw + vx);
        for(int x=0;x<W;x++){
            uint8_t pr=row[0],pg=row[1],pb=row[2],pa=row[3]; uint8_t r,g,b;
            if(pa==0){ r=g=b=0; }
            else if(pa==255){ r=pr; g=pg; b=pb; }
            else { r=(uint8_t)((pr*255 + (pa>>1))/pa); g=(uint8_t)((pg*255 + (pa>>1))/pa); b=(uint8_t)((pb*255 + (pa>>1))/pa); }
            uint16_t v=((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3);
            out[j++]=(uint8_t)(v & 0xFF); out[j++]=(uint8_t)(v>>8); row+=4;
        }
    } return out;
}

// USB robust sender --------------------------------------------------------------
static void build_header_fixed(uint8_t hdr[PACK], int format, int frame_len){
    memset(hdr,0,PACK);
    hdr[0]=0xDA; hdr[1]=0xDB; hdr[2]=0xDC; hdr[3]=0xDD; // magic
    hdr[4]=0x02; hdr[5]=0x00;   // ver=2
    hdr[6]=0x01; hdr[7]=0x00;   // cmd=1
    hdr[8]=0x40; hdr[9]=0x01;   // H=320
    hdr[10]=0xF0; hdr[11]=0x00; // W=240
    hdr[12]=format & 0xFF; hdr[13]=(format >> 8) & 0xFF; // format (2=RGB565 only)
    hdr[22]=frame_len & 0xFF; hdr[23]=(frame_len >> 8) & 0xFF; 
    hdr[24]=(frame_len >> 16) & 0xFF; hdr[25]=(frame_len >> 24) & 0xFF; // frame_len
    hdr[26]=0x00; hdr[27]=0x00; hdr[28]=0x00; hdr[29]=0x08; // extra
}
static void ctrl_nudge(libusb_device_handle *h, uint16_t wIndex){ (void)h; (void)wIndex; }
static int pick_iface_and_out_ep(libusb_device_handle *h,int want_iface,int *iface,unsigned char *ep_out){
    libusb_device *dev=libusb_get_device(h); struct libusb_config_descriptor* cfg=NULL; int rc=libusb_get_active_config_descriptor(dev,&cfg); if(rc) return rc;
    *iface=-1; *ep_out=0;
    for(int i=0;i<cfg->bNumInterfaces;i++){
        if(want_iface!=-1 && i!=want_iface) continue;
        const struct libusb_interface *itf=&cfg->interface[i];
        for(int a=0;a<itf->num_altsetting;a++){
            const struct libusb_interface_descriptor *alt=&itf->altsetting[a];
            for(int e=0;e<alt->bNumEndpoints;e++){
                const struct libusb_endpoint_descriptor *ep=&alt->endpoint[e];
                uint8_t type=ep->bmAttributes & 0x3; uint8_t addr=ep->bEndpointAddress; int is_out=((addr&0x80)==0);
                if(is_out && type==LIBUSB_TRANSFER_TYPE_INTERRUPT){ *iface=alt->bInterfaceNumber; *ep_out=addr; goto done; }
                if(is_out && type==LIBUSB_TRANSFER_TYPE_BULK && *ep_out==0){ *iface=alt->bInterfaceNumber; *ep_out=addr; }
            }
        }
    }
done:
    libusb_free_config_descriptor(cfg);
    return (*iface<0||*ep_out==0)? LIBUSB_ERROR_OTHER : 0;
}
static void ensure_claim(libusb_device_handle *h,int iface){
    if(libusb_kernel_driver_active(h,iface)==1) libusb_detach_kernel_driver(h,iface);
    int rc=libusb_claim_interface(h,iface);
    if(rc){ fprintf(stderr,"claim interface %d failed (%d)\n",iface,rc); exit(1); }
}
static void usb_soft_recover(libusb_device_handle *h,unsigned char ep_out){ libusb_clear_halt(h,ep_out); }
static int usb_reset_and_reclaim(libusb_device_handle *h,int *iface,unsigned char *ep_out,int want_iface){
    int rc=libusb_reset_device(h); if(rc) return rc; usleep(300*1000);
    int i; unsigned char e; rc=pick_iface_and_out_ep(h,want_iface,&i,&e); if(rc) return rc; *iface=i; *ep_out=e; ensure_claim(h,*iface); return 0;
}
static int usb_full_reopen(libusb_context **pctx,libusb_device_handle **ph,int want_iface,int *iface,unsigned char *ep_out){
    if(*ph){ libusb_close(*ph); *ph=NULL; } if(*pctx){ libusb_exit(*pctx); *pctx=NULL; }
    int rc=libusb_init(pctx); if(rc) return rc;
    for(int tries=0;tries<10;tries++){
        *ph=libusb_open_device_with_vid_pid(*pctx,VID,PID);
        if(*ph){ libusb_set_auto_detach_kernel_driver(*ph,1);
            rc=pick_iface_and_out_ep(*ph,want_iface,iface,ep_out);
            if(!rc){ ensure_claim(*ph,*iface); return 0; }
            libusb_close(*ph); *ph=NULL; }
        usleep(200*1000);
    }
    return LIBUSB_ERROR_NO_DEVICE;
}
static int out512_retry(libusb_context **pctx,libusb_device_handle **ph,int want_iface,int *iface,unsigned char *ep_out,const uint8_t *buf,int len){
    unsigned char pkt[PACK]; memset(pkt,0,sizeof pkt); if(len>PACK) len=PACK; memcpy(pkt,buf,len);
    for(int attempt=0;attempt<4;attempt++){
        int xfer=0; int r=libusb_interrupt_transfer(*ph,*ep_out,pkt,PACK,&xfer,CL_TIMEOUT);
        if(r==LIBUSB_ERROR_PIPE || r==LIBUSB_ERROR_TIMEOUT) r=libusb_bulk_transfer(*ph,*ep_out,pkt,PACK,&xfer,CL_TIMEOUT);
        if(r==0 && xfer==PACK) return 0;
        if(attempt==0){ usb_soft_recover(*ph,*ep_out); usleep(50*1000); }
        else if(attempt==1){ (void)usb_reset_and_reclaim(*ph,iface,ep_out,want_iface); usleep(150*1000); }
        else { int rc=usb_full_reopen(pctx,ph,want_iface,iface,ep_out); if(rc) return rc; }
    }
    return LIBUSB_ERROR_IO;
}

// Asset cache: background + images (static, APNG, or video) ---------------------
typedef struct {
    int asset_type; // 0=static, 1=apng, 2=video
    ApngAnim anim; 
    ImageRGBA stat;
    VideoDecoder video;
    int loaded;
    // playback knobs
    double speed; int64_t start_ms; int loop_mode; int loop_N;
} Asset;

static void asset_free(Asset *a){
    if(!a) return;
    if(a->asset_type == 1) apnganim_free(&a->anim);
    else if(a->asset_type == 0) free_imgrgba(&a->stat);
    else if(a->asset_type == 2) video_decoder_free(&a->video);
    memset(a,0,sizeof *a);
}

// Main ---------------------------------------------------------------------------
int main(void){
    struct sigaction sa = {0};
    sa.sa_handler = on_sighup;  sigaction(SIGHUP,  &sa, NULL);
    sa.sa_handler = on_sigterm; sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_sigterm; sigaction(SIGINT,  &sa, NULL);

    // Set process priority to be nice to other processes
    // This is a background display process, so it should have lower priority
    int nice_value = 10; // Higher nice value = lower priority (0-19 range)
    if(nice(nice_value) == -1) {
        fprintf(stderr, "Warning: Could not set process priority (nice %d)\n", nice_value);
    } else {
        fprintf(stderr, "Process priority set to nice %d (lower priority for background operation)\n", nice_value);
    }

    Layout L;
    if(load_layout("layout.cfg",&L)!=0){ fprintf(stderr,"Failed to load layout.cfg\n"); return 1; }

    // Compute FB and viewport
    compute_fb();
    // Use global FBW/FBH which are set to 400x400 for proper video centering
    int fbw=FBW, fbh=FBH;

    // Preload background as asset
    Asset bg={0};
    
    // Try video first if specified
    if(L.background_video[0] != 0){
        // Automatically rotate background 90° from text rotation
        int video_rotation = 0;  // Will be calculated based on text orientation
        
        // Calculate background rotation based on text orientation
        if(L.text_orient == ORIENT_LANDSCAPE) {
            if(L.text_landscape_ccw) {
                // Text: landscape CCW, Background: +90° (CW) from text
                video_rotation = 90;
                printf("[VIDEO] Text: landscape CCW, Background: +90° (CW) rotation\n");
            } else {
                // Text: landscape CW, Background: -90° (CCW) from text  
                video_rotation = -90;
                printf("[VIDEO] Text: landscape CW, Background: -90° (CCW) rotation\n");
            }
        } else {
            // Text: portrait, Background: +90° (landscape) from text
            video_rotation = 90;
            printf("[VIDEO] Text: portrait, Background: +90° (landscape) rotation\n");
        }
        if(video_decoder_load(L.background_video, &bg.video, video_rotation) == 0){
            bg.asset_type = 2; bg.loaded = 1;
            bg.speed = L.bg_video_speed; bg.start_ms = L.bg_video_start_ms;
            bg.loop_mode = L.bg_video_loop_mode; bg.loop_N = L.bg_video_loop_N;
            
            // Apply cache setting from config
            if(!L.video_cache_enabled){
                bg.video.cache_enabled = 0;
                fprintf(stderr,"[VIDEO] Caching disabled by config\n");
            } else if(bg.video.cache_enabled){
                fprintf(stderr,"[VIDEO] Caching enabled for short video\n");
            }
            
            fprintf(stderr,"[VIDEO] background: %dx%d, duration=%lldms, loop_mode=%d (%s)\n",
                    bg.video.width, bg.video.height, (long long)bg.video.duration_ms, bg.loop_mode, L.background_video);
        } else {
            fprintf(stderr,"Failed to load video background: %s\n", L.background_video);
            return 1;
        }
    } else if(L.background_png[0] != 0){
        // Try APNG precompose (rotate all frames at load if background_flip)
        int apng_stat = apng_load_precompose(L.background_png, &bg.anim, L.background_flip);
        if(apng_stat==0 && bg.anim.is_apng){
            bg.asset_type=1; bg.loaded=1;
            bg.speed = L.bg_apng_speed; bg.start_ms = L.bg_apng_start_ms;
            bg.loop_mode = L.bg_apng_loop_mode; bg.loop_N = L.bg_apng_loop_N;
            fprintf(stderr,"[APNG] background: %u frames, plays=%u, total=%ums (%s)\n",
                    bg.anim.num_frames, bg.anim.plays, bg.anim.total_ms, L.background_png);
        } else if(apng_stat==1){
            // Static PNG fallback
            ImageRGBA s = load_png_rgba_stb(L.background_png);
            if(L.background_flip) rotate180_rgba(s.rgba, s.w, s.h);
            bg.asset_type=0; bg.stat=s; bg.loaded=1;
        } else {
            fprintf(stderr,"Failed to load background: %s\n", L.background_png);
            return 1;
        }
    } else {
        fprintf(stderr,"No background specified\n");
        return 1;
    }

    // Preload image layers
    Asset *imgA=(Asset*)calloc(L.n_imgs,sizeof(Asset));
    for(int i=0;i<L.n_imgs;i++){
        ApngAnim anim={0};
        int st = apng_load_precompose(L.imgs[i].path, &anim, 0);
        if(st==0 && anim.is_apng){
            imgA[i].asset_type=1; imgA[i].anim=anim; imgA[i].loaded=1;
            imgA[i].speed=L.imgs[i].apng_speed; imgA[i].start_ms=L.imgs[i].apng_start_ms;
            imgA[i].loop_mode=L.imgs[i].apng_loop_mode; imgA[i].loop_N=L.imgs[i].apng_loop_N;
            fprintf(stderr,"[APNG] image[%d]: %u frames, plays=%u, total=%ums (%s)\n",
                    i, anim.num_frames, anim.plays, anim.total_ms, L.imgs[i].path);
        } else if(st==1){
            ImageRGBA s = load_png_rgba_stb(L.imgs[i].path);
            imgA[i].asset_type=0; imgA[i].stat=s; imgA[i].loaded=1;
        } else {
            fprintf(stderr,"Failed to load image layer: %s\n", L.imgs[i].path);
            imgA[i].loaded=0;
        }
    }

    // USB open
    libusb_context* ctx=NULL; libusb_device_handle* h=NULL;
    if(libusb_init(&ctx)){ fprintf(stderr,"libusb_init failed\n"); return 1; }
    h=libusb_open_device_with_vid_pid(ctx,VID,PID);
    if(!h){ fprintf(stderr,"device %04x:%04x not found\n",VID,PID); libusb_exit(ctx); return 1; }
    libusb_set_auto_detach_kernel_driver(h,1);
    int iface=-1; unsigned char ep_out=0;
    if(pick_iface_and_out_ep(h,L.iface,&iface,&ep_out)!=0){
        fprintf(stderr,"No OUT endpoint%s\n",(L.iface!=-1?" on requested iface":"")); libusb_close(h); libusb_exit(ctx); return 1;
    }
    ensure_claim(h,iface);
    uint16_t wIndex=(uint16_t)iface;
    uint8_t hdr[PACK];
    // Use video's native frame rate if available, otherwise use config FPS
    int target_fps = L.fps;
    int original_fps = target_fps;
    if(bg.asset_type == 2 && bg.video.loaded && bg.video.fps_detected){
        target_fps = (int)bg.video.video_fps;
        original_fps = target_fps;
        printf("[VIDEO] Using native frame rate: %d FPS (from video)\n", target_fps);
    } else if(L.fps > 0){
        printf("[VIDEO] Using config frame rate: %d FPS\n", L.fps);
    }
    
    int period_ms=(target_fps>0)?(1000/target_fps):0;

    Metrics M; metrics_init(&M);
    int frame_idx=0;
    uint64_t t0 = now_monotonic_ms();
    
    // Performance monitoring for frame rate adjustment
    uint64_t last_perf_check = t0;
    int perf_check_interval = 100; // Check performance every 100 frames
    
    // Optimization: reuse framebuffer to reduce allocations
    uint8_t *fb = fb_rgba_alloc_clear(fbw, fbh);
    if(!fb) die("Failed to allocate framebuffer");

    do{
        // Clear framebuffer instead of reallocating
        memset(fb, 0, (size_t)fbw * fbh * 4);

        // Update metrics (blocking sample on 1st frame if one-shot)
        update_metrics(&M, (frame_idx==0 && period_ms==0));

        // Background position
        int bgx=0,bgy=0;
        if(bg.asset_type == 2){ // Video
            // Video frames are 320x320 square - center them in the framebuffer
            bgx = (fbw - 320) / 2;  // Center horizontally (video width = 320)
            bgy = (fbh - 320) / 2;  // Center vertically (video height = 320)
            

            uint64_t elapsed = now_monotonic_ms() - t0 + (uint64_t)(bg.start_ms>=0? bg.start_ms : 0);
            elapsed = (uint64_t)((double)elapsed * bg.speed);
            
            // Handle looping
            if(bg.video.duration_ms > 0){
                if(bg.loop_mode == 1){ // infinite
                    elapsed = elapsed % bg.video.duration_ms;
                } else if(bg.loop_mode == 2){ // once
                    if(elapsed >= (uint64_t)bg.video.duration_ms) elapsed = (uint64_t)bg.video.duration_ms - 1;
                } else if(bg.loop_mode == 3){ // custom N
                    uint64_t total_duration = bg.video.duration_ms * bg.loop_N;
                    if(elapsed >= total_duration) elapsed = total_duration - 1;
                    else elapsed = elapsed % bg.video.duration_ms;
                }
            }
            
            // Print video position every 100 frames
            if(frame_idx % 100 == 0){
                printf("[VIDEO] Position: %llu/%llu ms (%.1f%%) - Frame %d - Centered at (%d,%d)\n", 
                       (unsigned long long)elapsed, (unsigned long long)bg.video.duration_ms,
                       (float)elapsed * 100.0f / bg.video.duration_ms, frame_idx, bgx, bgy);
            }
            
            uint8_t *frame_rgba = NULL;
            int frame_w = 0, frame_h = 0;
            if(video_decoder_get_frame(&bg.video, elapsed, &frame_rgba, &frame_w, &frame_h) == 0){
                // Video frames are 320x320 square and centered
                // Just blit them directly to the centered position
                blit_png_into_fb(fb, fbw, fbh, frame_rgba, frame_w, frame_h, bgx, bgy, -1, 1.0f);
            }
        } else if(bg.asset_type == 1){ // APNG
            int bw=(int)bg.anim.canvas_w, bh=(int)bg.anim.canvas_h;
            bgx = L.bg_x_mode ? (fbw - bw)/2 : L.bg_x;
            bgy = L.bg_y_mode ? (fbh - bh)/2 : L.bg_y;
            if(bgx<-bw) bgx=-bw;
            if(bgy<-bh) bgy=-bh;
            if(bgx>fbw) bgx=fbw;
            if(bgy>fbh) bgy=fbh;

            uint64_t elapsed = now_monotonic_ms() - t0 + (uint64_t)(bg.start_ms>=0? bg.start_ms : 0);
            unsigned rem_ms=0;
            unsigned idx = apng_pick_frame(&bg.anim, elapsed, bg.speed, bg.loop_mode, bg.loop_N, &rem_ms);
            const uint8_t *fr = bg.anim.frame_rgba[idx];
            blit_png_into_fb(fb,fbw,fbh, fr, bw,bh, bgx,bgy, -1, 1.0f);
        } else { // Static PNG
            int bw=bg.stat.w, bh=bg.stat.h;
            bgx = L.bg_x_mode ? (fbw - bw)/2 : L.bg_x;
            bgy = L.bg_y_mode ? (fbh - bh)/2 : L.bg_y;
            if(bgx<-bw) bgx=-bw;
            if(bgy<-bh) bgy=-bh;
            if(bgx>fbw) bgx=fbw;
            if(bgy>fbh) bgy=fbh;
            blit_png_into_fb(fb,fbw,fbh, bg.stat.rgba, bw,bh, bgx,bgy, -1, 1.0f);
        }

        // Image layers
        for(int i=0;i<L.n_imgs;i++){
            if(!imgA[i].loaded) continue;
            if(imgA[i].asset_type == 1){ // APNG
                const ApngAnim *A=&imgA[i].anim;
                uint64_t elapsed = now_monotonic_ms() - t0 + (uint64_t)(imgA[i].start_ms>=0? imgA[i].start_ms : 0);
                unsigned rem_ms=0;
                unsigned idx = apng_pick_frame(A, elapsed, imgA[i].speed, imgA[i].loop_mode, imgA[i].loop_N, &rem_ms);
                const uint8_t *fr = A->frame_rgba[idx];
                blit_png_into_fb(fb,fbw,fbh, fr, (int)A->canvas_w,(int)A->canvas_h, L.imgs[i].x, L.imgs[i].y, L.imgs[i].alpha, L.imgs[i].scale>0?L.imgs[i].scale:1.0f);
            } else { // Static PNG
                blit_png_into_fb(fb,fbw,fbh, imgA[i].stat.rgba, imgA[i].stat.w,imgA[i].stat.h, L.imgs[i].x, L.imgs[i].y, L.imgs[i].alpha, L.imgs[i].scale>0?L.imgs[i].scale:1.0f);
            }
        }

        // Overlays
        for(int i=0;i<L.n_overlays;i++) draw_overlay_ui(fb,fbw,fbh,L.overlays[i],L.text_orient,L.text_flip,&L);

        // Text
        for(int i=0;i<L.n_texts;i++) draw_text_ttf(fb,fbw,fbh,&L.texts[i],L.text_orient,L.text_flip,&L,&M);

        // Prepare frame data
        int vx,vy; compute_viewport(&L,&vx,&vy);
        
        
        // Convert viewport to RGB565 (LCD only supports this format)
        uint8_t *frame_data = viewport_to_rgb565(fb,fbw,fbh,vx,vy);
        int frame_len = FRAME_LEN;
        int format = 2; // RGB565
        
        // Build header with correct format and length
        build_header_fixed(hdr, format, frame_len);
        
        // Send with frame validation
        ctrl_nudge(h,wIndex);
        int rc=out512_retry(&ctx,&h,L.iface,&iface,&ep_out,hdr,sizeof hdr);
        if(rc){ fprintf(stderr,"header send failed rc=%d\n",rc); free(frame_data); break; }
        ctrl_nudge(h,wIndex);
        
        // Track USB transmission success
        int usb_errors = 0;
        for(int off=0; off<frame_len; off+=PACK){
            ctrl_nudge(h,wIndex);
            int n=(frame_len-off>=PACK)?PACK:(frame_len-off);
            rc=out512_retry(&ctx,&h,L.iface,&iface,&ep_out,frame_data+off,n);
            if(rc){ 
                usb_errors++;
                fprintf(stderr,"data send failed at off=%d rc=%d (error %d)\n",off,rc,usb_errors); 
                if(usb_errors > 3){
                    fprintf(stderr,"Too many USB errors, stopping\n");
                    free(frame_data); 
                    goto tx_done; 
                }
            }
            ctrl_nudge(h,wIndex);
        }
        
        // If we had USB errors, increase frame interval to reduce load
        if(usb_errors > 0 && target_fps > 6){
            target_fps = 6; // Very conservative FPS
            period_ms = (1000/target_fps);
            printf("[VIDEO] USB errors detected, reducing to %d FPS for stability\n", target_fps);
        }
        free(frame_data);

        if(period_ms>0){ struct timespec ts; ts.tv_sec=period_ms/1000; ts.tv_nsec=(long)(period_ms%1000)*1000000L; nanosleep(&ts,NULL); }
        frame_idx++;
        
        // Performance monitoring - check if we need to reduce frame rate
        if(frame_idx % perf_check_interval == 0){
            uint64_t now = now_monotonic_ms();
            uint64_t elapsed = now - last_perf_check;
            uint64_t expected_time = (uint64_t)perf_check_interval * period_ms;
            
            // If we're taking significantly longer than expected, consider reducing FPS
            if(elapsed > expected_time * 1.3f && target_fps > original_fps/2){ // More sensitive detection
                if(target_fps > 16) {
                    target_fps = 16; // First reduction
                    printf("[VIDEO] Performance issue detected, reducing to %d FPS\n", target_fps);
                } else if(target_fps > 12) {
                    target_fps = 12; // Second reduction
                    printf("[VIDEO] Performance issue detected, reducing to %d FPS\n", target_fps);
                } else if(target_fps > 8) {
                    target_fps = 8; // Third reduction
                    printf("[VIDEO] Performance issue detected, reducing to %d FPS\n", target_fps);
                }
                period_ms = (target_fps > 0) ? (1000/target_fps) : 0;
            }
            
            // USB bandwidth monitoring - if we're having transmission issues, reduce FPS further
            if(target_fps > 6 && frame_idx > 30){ // Earlier detection, lower minimum
                // Check if we're consistently behind schedule (USB bottleneck)
                if(elapsed > expected_time * 1.15f){ // More sensitive detection
                    if(target_fps > 12) {
                        target_fps = 12; // First reduction
                        printf("[VIDEO] USB bandwidth issue detected, reducing to %d FPS\n", target_fps);
                    } else if(target_fps > 8) {
                        target_fps = 8; // Second reduction
                        printf("[VIDEO] USB bandwidth issue detected, reducing to %d FPS\n", target_fps);
                    } else if(target_fps > 6) {
                        target_fps = 6; // Final reduction
                        printf("[VIDEO] USB bandwidth issue detected, reducing to %d FPS\n", target_fps);
                    }
                    period_ms = (1000/target_fps);
                }
            }
            
            last_perf_check = now;
        }

        if (g_stop) break;

        if (g_reload) {
           // will add when or if I want it, restarting the service is fine at the moment
            g_reload = 0;
        }

    } while(period_ms>0 && L.once==0);

tx_done:
    // Free the reused framebuffer
    if(fb) free(fb);
    
    if(h && iface>=0) libusb_release_interface(h,iface);
    if(h) libusb_close(h);
    if(ctx) libusb_exit(ctx);

    // Free assets
    if(bg.loaded) asset_free(&bg);
    for(int i=0;i<L.n_imgs;i++) asset_free(&imgA[i]);
    free(imgA);

    for(int i=0;i<L.n_texts;i++){ free(L.texts[i].text); if(L.texts[i].ttf_path) free(L.texts[i].ttf_path); }
    for(int i=0;i<L.n_imgs;i++) free(L.imgs[i].path);
    free(L.texts); free(L.overlays); free(L.imgs);

    puts("Frame sent.");
    return 0;
}
