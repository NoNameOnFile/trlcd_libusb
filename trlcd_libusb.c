// trlcd_libusb.c — USB LCD compositor with PNG/APNG, UTF-8 TTF text, overlays,
// robust USB retries, big FB + viewport. APNG frames are precomposed with
// correct blend/dispose and timed playback.
//
// Build:
//   gcc -O2 -Wall trlcd_libusb.c lodepng.c -lusb-1.0 -lm -o trlcd_libusb
//
// Requires (in same directory):
//   - stb_image.h        (PNG decode for static images)
//   - stb_truetype.h     (TTF text rasterization)
//   - lodepng.h, lodepng.c (APNG frame decode)
//
// Notes:
//   * Background offset can be numeric or "center" (layout.cfg)
//   * Text is TTF-only; bitmap font removed.
//   * Tokens: %CPU_TEMP% %CPU_USAGE% %MEM_USED% %MEM_FREE% %GPU_TEMP% %GPU_USAGE% %TIME% %DATE%

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

    // Debug
    int debug;
} Layout;

static UiOrient parse_orient(const char *v){
    if(v && strcasecmp(v,"landscape")==0) return ORIENT_LANDSCAPE;
    return ORIENT_PORTRAIT;
}
static int parse_rect(const char *s, int *x,int *y,int *w,int *h){
    int X=0,Y=0,Wd=0,Ht=0;
    int n = sscanf(s," %d , %d , %d , %d ",&X,&Y,&Wd,&Ht);
    if(n!=4) return -1; *x=X; *y=Y; *w=Wd; *h=Ht; return 0;
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
    L->fb_scale_percent=150;
    L->viewport_x=-1; L->viewport_y=-1;
    L->default_ttf[0]=0; L->default_ttf_px=0;
    L->bg_apng_speed=1.0; L->bg_apng_start_ms=0; L->bg_apng_loop_mode=0; L->bg_apng_loop_N=0;
    L->debug=0;
}
static void add_overlay(Layout *L, Overlay ov){
    L->overlays=(Overlay*)realloc(L->overlays,(L->n_overlays+1)*sizeof(Overlay));
    if(!L->overlays) die("realloc overlays"); L->overlays[L->n_overlays++]=ov;
}
static void add_text(Layout *L, TextItem ti){
    L->texts=(TextItem*)realloc(L->texts,(L->n_texts+1)*sizeof(TextItem));
    if(!L->texts) die("realloc texts"); L->texts[L->n_texts++]=ti;
}
static void add_img(Layout *L, ImgLayer im){
    L->imgs=(ImgLayer*)realloc(L->imgs,(L->n_imgs+1)*sizeof(ImgLayer));
    if(!L->imgs) die("realloc imgs"); L->imgs[L->n_imgs++]=im;
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
            else if(!strcmp(k,"background_flip")) L->background_flip=atoi(v);
            else if(!strcmp(k,"background_x")){ if(strcasecmp(v,"center")==0)L->bg_x_mode=1; else{L->bg_x_mode=0; L->bg_x=atoi(v);} }
            else if(!strcmp(k,"background_y")){ if(strcasecmp(v,"center")==0)L->bg_y_mode=1; else{L->bg_y_mode=0; L->bg_y=atoi(v);} }

            else if(!strcmp(k,"text_orientation")) L->text_orient=parse_orient(v);
            else if(!strcmp(k,"text_flip")) L->text_flip=atoi(v);
            else if(!strcmp(k,"text_landscape_dir")) L->text_landscape_ccw=(strcasecmp(v,"ccw")==0)?1:0;

            else if(!strcmp(k,"fb_scale_percent")) L->fb_scale_percent=atoi(v);
            else if(!strcmp(k,"viewport_x")) L->viewport_x=(strcasecmp(v,"center")==0)?-1:atoi(v);
            else if(!strcmp(k,"viewport_y")) L->viewport_y=(strcasecmp(v,"center")==0)?-1:atoi(v);

            else if(!strcmp(k,"fps")) L->fps=atoi(v);
            else if(!strcmp(k,"once")) L->once=atoi(v);
            else if(!strcmp(k,"iface")) L->iface=atoi(v);
            else if(!strcmp(k,"debug")) L->debug=atoi(v);

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
                if(cur_text.ttf_path) free(cur_text.ttf_path); cur_text.ttf_path=strdup(v);
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

    if(L->background_png[0]==0){ fprintf(stderr,"layout.cfg missing 'background_png='\n"); return -1; }
    if(L->fb_scale_percent<100) L->fb_scale_percent=100;
    if(L->bg_apng_speed<=0) L->bg_apng_speed=1.0;
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
    char time_hhmm[8];
    char date_ymd[16];
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
    for(int i=0;i<32;i++){ char p[128]; snprintf(p,sizeof p,"/sys/class/thermal/thermal_zone%d/temp",i);
        long long v; if(read_file_ll(p,&v)==0){ if(v>1000) v=(v+5)/10; if(v>best) best=v; } }
    for(int h=0;h<16;h++)for(int t=1;t<=8;t++){ char p[160]; snprintf(p,sizeof p,"/sys/class/hwmon/hwmon%d/temp%d_input",h,t);
        long long v; if(read_file_ll(p,&v)==0){ if(v>1000) v=v/100; if(v>best) best=v; } }
    if(best<0) return -1; *out_c = best/10.0f; return 0;
}
static int read_cpu_totals(uint64_t *idle, uint64_t *total){
    FILE *f=fopen("/proc/stat","r"); if(!f) return -1; char line[256];
    if(!fgets(line,sizeof line,f)){ fclose(f); return -1; } fclose(f);
    unsigned long long u=0,n=0,s=0,i=0,w=0,irq=0,sirq=0,st=0;
    int c=sscanf(line,"cpu %llu %llu %llu %llu %llu %llu %llu %llu",&u,&n,&s,&i,&w,&irq,&sirq,&st);
    if(c<4) return -1; *idle = i+w; *total = u+n+s+*idle+irq+sirq+st; return 0;
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
        for(char *p=nm;*p;p++) if(*p=='\n'||*p=='\r') *p=0; int ok=0;
        for(size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) if(!strcasecmp(nm,names[i])){ ok=1; break; }
        if(!ok) continue;
        for(int t=1;t<=8;t++){ char tp[160]; snprintf(tp,sizeof tp,"/sys/class/hwmon/hwmon%d/temp%d_input",h,t);
            long long v; if(read_file_ll(tp,&v)==0){ float c=(v>=1000)?(v/1000.0f):(v/1.0f); if(c>best) best=c; } }
    }
    if(best<0) return -1; *out_c=best; return 0;
}
static int get_gpu_usage_pct(float *out_pct){
    DIR *d=opendir("/sys/class/drm"); if(!d) return -1; struct dirent *de; float best=-1.0f;
    while((de=readdir(d))){ if(strncmp(de->d_name,"card",4)!=0) continue; char p1[256]; unsigned long long v=0;
        snprintf(p1,sizeof p1,"/sys/class/drm/%s/device/gpu_busy_percent",de->d_name);
        if(read_file_u64(p1,&v)==0){ if((float)v>best) best=(float)v; continue; }
        snprintf(p1,sizeof p1,"/sys/class/drm/%s/device/busy_percent",de->d_name);
        if(read_file_u64(p1,&v)==0){ if((float)v>best) best=(float)v; continue; }
        snprintf(p1,sizeof p1,"/sys/class/drm/%s/device/gt_busy_percent",de->d_name);
        if(read_file_u64(p1,&v)==0){ if((float)v>best) best=(float)v; continue; }
    }
    closedir(d); if(best<0) return -1; if(best>100.0f) best=100.0f; *out_pct=best; return 0;
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
    time_t now=time(NULL); struct tm lt; localtime_r(&now,&lt);
    snprintf(m->time_hhmm,sizeof m->time_hhmm,"%02d:%02d", lt.tm_hour, lt.tm_min);
    snprintf(m->date_ymd,sizeof m->date_ymd,"%04d-%02d-%02d", lt.tm_year+1900, lt.tm_mon+1, lt.tm_mday);

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
    if(x1>LW)x1=LW; if(y1>LH)y1=LH;
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
    strncpy(c->path,path,sizeof(c->path)-1); c->px=px; c->ttf_data=data; c->ttf_size=(size_t)sz; c->info=info;
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
static int rd_read(Reader *r, unsigned char *out, size_t n){ if(r->left<n) return -1; memcpy(out,r->p,n); r->p+=n; r->left-=n; return 0; }
static int rd_skip(Reader *r, size_t n){ if(r->left<n) return -1; r->p+=n; r->left-=n; return 0; }

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
static const unsigned char* chunk_dataptr(const Chunk *ch){ uint32_t len=be32r(ch->data); return ch->data+8; }
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
    unsigned acTL_frames=0, acTL_plays=0;
    int saw_acTL=0, saw_IDAT_or_fd=0;

    typedef struct {
        unsigned w,h,x,y, delay_num, delay_den; unsigned char dispose_op, blend_op;
        ByteVec idata; int in_use;
    } FrameBuild;

    FrameBuild cur; memset(&cur,0,sizeof cur); bv_init(&cur.idata);
    unsigned canvas_w=0, canvas_h=0;
    unsigned num_frames=0;

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
            acTL_frames = be32r(data+0); acTL_plays = be32r(data+4);
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

// FB/Viewport & RGB565 -----------------------------------------------------------
static int FBW= (W*3)/2, FBH= (H*3)/2;
static void compute_fb(const Layout *L){ int p=(L->fb_scale_percent<100)?100:L->fb_scale_percent; FBW=(W*p+99)/100; FBH=(H*p+99)/100; }
static void compute_viewport(const Layout *L, int *vx, int *vy){
    int x=(L->viewport_x<0)?(FBW - W)/2 : L->viewport_x;
    int y=(L->viewport_y<0)?(FBH - H)/2 : L->viewport_y;
    if(x<0)x=0; if(y<0)y=0; if(x>FBW-W)x=FBW-W; if(y>FBH-H)y=FBH-H; *vx=x; *vy=y;
}
static uint8_t* viewport_to_rgb565(const uint8_t *fb,int fbw,int fbh,int vx,int vy){
    (void)fbh; size_t px=(size_t)W*H; uint8_t *out=(uint8_t*)malloc(px*2); if(!out) die("malloc565");
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
static void build_header_fixed(uint8_t hdr[PACK]){
    memset(hdr,0,PACK);
    hdr[0]=0xDA; hdr[1]=0xDB; hdr[2]=0xDC; hdr[3]=0xDD; // magic
    hdr[4]=0x02; hdr[5]=0x00;   // ver=2
    hdr[6]=0x01; hdr[7]=0x00;   // cmd=1
    hdr[8]=0xF0; hdr[9]=0x00;   // H=240
    hdr[10]=0x40; hdr[11]=0x01; // W=320
    hdr[12]=0x02; hdr[13]=0x00; // fmt=2 (RGB565)
    hdr[22]=0x00; hdr[23]=0x58; hdr[24]=0x02; hdr[25]=0x00; // frame_len = 0x00025800
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

// Asset cache: background + images (static or APNG) ------------------------------
typedef struct {
    int is_anim; ApngAnim anim; ImageRGBA stat;
    int loaded;
    // playback knobs
    double speed; int64_t start_ms; int loop_mode; int loop_N;
} Asset;

static void asset_free(Asset *a){
    if(!a) return;
    if(a->is_anim) apnganim_free(&a->anim);
    else free_imgrgba(&a->stat);
    memset(a,0,sizeof *a);
}

// Main ---------------------------------------------------------------------------
int main(void){
    struct sigaction sa = {0};
    sa.sa_handler = on_sighup;  sigaction(SIGHUP,  &sa, NULL);
    sa.sa_handler = on_sigterm; sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_sigterm; sigaction(SIGINT,  &sa, NULL);

    Layout L;
    if(load_layout("layout.cfg",&L)!=0){ fprintf(stderr,"Failed to load layout.cfg\n"); return 1; }

    // Compute FB and viewport
    int FBW_local, FBH_local;
    compute_fb(&L);
    FBW_local = (W * L.fb_scale_percent + 99) / 100;
    FBH_local = (H * L.fb_scale_percent + 99) / 100;
    int fbw=FBW_local, fbh=FBH_local;

    // Preload background as asset
    Asset bg={0};
    // Try APNG precompose (rotate all frames at load if background_flip)
    int apng_stat = apng_load_precompose(L.background_png, &bg.anim, L.background_flip);
    if(apng_stat==0 && bg.anim.is_apng){
        bg.is_anim=1; bg.loaded=1;
        bg.speed = L.bg_apng_speed; bg.start_ms = L.bg_apng_start_ms;
        bg.loop_mode = L.bg_apng_loop_mode; bg.loop_N = L.bg_apng_loop_N;
        fprintf(stderr,"[APNG] background: %u frames, plays=%u, total=%ums (%s)\n",
                bg.anim.num_frames, bg.anim.plays, bg.anim.total_ms, L.background_png);
    } else if(apng_stat==1){
        // Static PNG fallback
        ImageRGBA s = load_png_rgba_stb(L.background_png);
        if(L.background_flip) rotate180_rgba(s.rgba, s.w, s.h);
        bg.is_anim=0; bg.stat=s; bg.loaded=1;
    } else {
        fprintf(stderr,"Failed to load background: %s\n", L.background_png);
        return 1;
    }

    // Preload image layers
    Asset *imgA=(Asset*)calloc(L.n_imgs,sizeof(Asset));
    for(int i=0;i<L.n_imgs;i++){
        ApngAnim anim={0};
        int st = apng_load_precompose(L.imgs[i].path, &anim, 0);
        if(st==0 && anim.is_apng){
            imgA[i].is_anim=1; imgA[i].anim=anim; imgA[i].loaded=1;
            imgA[i].speed=L.imgs[i].apng_speed; imgA[i].start_ms=L.imgs[i].apng_start_ms;
            imgA[i].loop_mode=L.imgs[i].apng_loop_mode; imgA[i].loop_N=L.imgs[i].apng_loop_N;
            fprintf(stderr,"[APNG] image[%d]: %u frames, plays=%u, total=%ums (%s)\n",
                    i, anim.num_frames, anim.plays, anim.total_ms, L.imgs[i].path);
        } else if(st==1){
            ImageRGBA s = load_png_rgba_stb(L.imgs[i].path);
            imgA[i].is_anim=0; imgA[i].stat=s; imgA[i].loaded=1;
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
    uint8_t hdr[PACK]; build_header_fixed(hdr);
    int period_ms=(L.fps>0)?(1000/L.fps):0;

    Metrics M; metrics_init(&M);
    int frame_idx=0;
    uint64_t t0 = now_monotonic_ms();

    do{
        uint8_t *fb = fb_rgba_alloc_clear(fbw, fbh);

        // Update metrics (blocking sample on 1st frame if one-shot)
        update_metrics(&M, (frame_idx==0 && period_ms==0));

        // Background position
        int bgx=0,bgy=0;
        if(bg.is_anim){
            int bw=(int)bg.anim.canvas_w, bh=(int)bg.anim.canvas_h;
            bgx = L.bg_x_mode ? (fbw - bw)/2 : L.bg_x;
            bgy = L.bg_y_mode ? (fbh - bh)/2 : L.bg_y;
            if(bgx<-bw) bgx=-bw; if(bgy<-bh) bgy=-bh; if(bgx>fbw) bgx=fbw; if(bgy>fbh) bgy=fbh;

            uint64_t elapsed = now_monotonic_ms() - t0 + (uint64_t)(bg.start_ms>=0? bg.start_ms : 0);
            unsigned rem_ms=0;
            unsigned idx = apng_pick_frame(&bg.anim, elapsed, bg.speed, bg.loop_mode, bg.loop_N, &rem_ms);
            const uint8_t *fr = bg.anim.frame_rgba[idx];
            blit_png_into_fb(fb,fbw,fbh, fr, bw,bh, bgx,bgy, -1, 1.0f);
        } else {
            int bw=bg.stat.w, bh=bg.stat.h;
            bgx = L.bg_x_mode ? (fbw - bw)/2 : L.bg_x;
            bgy = L.bg_y_mode ? (fbh - bh)/2 : L.bg_y;
            if(bgx<-bw) bgx=-bw; if(bgy<-bh) bgy=-bh; if(bgx>fbw) bgx=fbw; if(bgy>fbh) bgy=fbh;
            blit_png_into_fb(fb,fbw,fbh, bg.stat.rgba, bw,bh, bgx,bgy, -1, 1.0f);
        }

        // Image layers
        for(int i=0;i<L.n_imgs;i++){
            if(!imgA[i].loaded) continue;
            if(imgA[i].is_anim){
                const ApngAnim *A=&imgA[i].anim;
                uint64_t elapsed = now_monotonic_ms() - t0 + (uint64_t)(imgA[i].start_ms>=0? imgA[i].start_ms : 0);
                unsigned rem_ms=0;
                unsigned idx = apng_pick_frame(A, elapsed, imgA[i].speed, imgA[i].loop_mode, imgA[i].loop_N, &rem_ms);
                const uint8_t *fr = A->frame_rgba[idx];
                blit_png_into_fb(fb,fbw,fbh, fr, (int)A->canvas_w,(int)A->canvas_h, L.imgs[i].x, L.imgs[i].y, L.imgs[i].alpha, L.imgs[i].scale>0?L.imgs[i].scale:1.0f);
            } else {
                blit_png_into_fb(fb,fbw,fbh, imgA[i].stat.rgba, imgA[i].stat.w,imgA[i].stat.h, L.imgs[i].x, L.imgs[i].y, L.imgs[i].alpha, L.imgs[i].scale>0?L.imgs[i].scale:1.0f);
            }
        }

        // Overlays
        for(int i=0;i<L.n_overlays;i++) draw_overlay_ui(fb,fbw,fbh,L.overlays[i],L.text_orient,L.text_flip,&L);

        // Text
        for(int i=0;i<L.n_texts;i++) draw_text_ttf(fb,fbw,fbh,&L.texts[i],L.text_orient,L.text_flip,&L,&M);

        // Viewport -> RGB565
        int vx,vy; compute_viewport(&L,&vx,&vy);
        uint8_t *rgb565 = viewport_to_rgb565(fb,fbw,fbh,vx,vy);

        // Send
        ctrl_nudge(h,wIndex);
        int rc=out512_retry(&ctx,&h,L.iface,&iface,&ep_out,hdr,sizeof hdr);
        if(rc){ fprintf(stderr,"header send failed rc=%d\n",rc); free(rgb565); free(fb); break; }
        ctrl_nudge(h,wIndex);
        for(int off=0; off<FRAME_LEN; off+=PACK){
            ctrl_nudge(h,wIndex);
            int n=(FRAME_LEN-off>=PACK)?PACK:(FRAME_LEN-off);
            rc=out512_retry(&ctx,&h,L.iface,&iface,&ep_out,rgb565+off,n);
            if(rc){ fprintf(stderr,"data send failed at off=%d rc=%d\n",off,rc); free(rgb565); free(fb); goto tx_done; }
            ctrl_nudge(h,wIndex);
        }
        free(rgb565);
        free(fb);

        if(period_ms>0){ struct timespec ts; ts.tv_sec=period_ms/1000; ts.tv_nsec=(long)(period_ms%1000)*1000000L; nanosleep(&ts,NULL); }
        frame_idx++;

        if (g_stop) break;

        if (g_reload) {
           // will add when or if I want it, restarting the service is fine at the moment
            g_reload = 0;
        }

    } while(period_ms>0 && L.once==0);

tx_done:
    if(h && iface>=0) libusb_release_interface(h,iface);
    if(h) libusb_close(h); if(ctx) libusb_exit(ctx);

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
