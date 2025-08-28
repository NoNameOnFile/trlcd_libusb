// trlcd_libusb.c — PNG compositor with big framebuffer, movable background,
// per-text rotation/flip overrides, robust USB sender, and dynamic tokens:
//   %CPU_TEMP%  -> "NN°C"
//   %CPU_USAGE% -> "NN%"
// Build (Linux):
//   gcc -O2 -Wall trlcd_libusb.c -lusb-1.0 -o trlcd_libusb
//
// Requires stb_image.h in the same directory (header-only).
// David Pope

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

// ---------------- Panel + Transport ----------------
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

// ---------------- Tiny 5x7 ASCII Bitmap Font ----------------
static const uint8_t font5x7[95][7] = {
    {0,0,0,0,0,0,0},{0x04,0x04,0x04,0x04,0,0,0x04},{0x0A,0x0A,0,0,0,0,0},
    {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0},{0x04,0x1E,0x05,0x0E,0x14,0x0F,0x04},
    {0x19,0x19,0x02,0x04,0x08,0x13,0x13},{0x0C,0x12,0x14,0x08,0x15,0x12,0x0D},
    {0x06,0x04,0x08,0,0,0,0},{0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08},{0,0x04,0x15,0x0E,0x15,0x04,0},
    {0,0x04,0x04,0x1F,0x04,0x04,0},{0,0,0,0,0x06,0x04,0x08},
    {0,0,0,0x1F,0,0,0},{0,0,0,0,0x06,0x06,0},
    {0x01,0x01,0x02,0x04,0x08,0x10,0x10},{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},{0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},
    {0x1F,0x01,0x02,0x06,0x01,0x11,0x0E},{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},{0,0x06,0x06,0,0x06,0x06,0},
    {0,0x06,0x06,0,0x06,0x04,0x08},{0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    {0,0,0x1F,0,0x1F,0,0},{0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    {0x0E,0x11,0x01,0x06,0x04,0,0x04},{0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},{0x01,0x01,0x01,0x01,0x11,0x11,0x0E},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},{0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},{0x10,0x10,0x08,0x04,0x02,0x01,0x01},
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},{0x04,0x0A,0x11,0,0,0,0},{0,0,0,0,0,0,0x1F},
    {0x0C,0x04,0x02,0,0,0,0},{0,0,0x0E,0x01,0x0F,0x11,0x0F},
    {0x10,0x10,0x16,0x19,0x11,0x11,0x1E},{0,0,0x0E,0x11,0x10,0x11,0x0E},
    {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F},{0,0,0x0E,0x11,0x1F,0x10,0x0E},
    {0x06,0x08,0x1E,0x08,0x08,0x08,0x08},{0,0x0F,0x11,0x11,0x0F,0x01,0x0E},
    {0x10,0x10,0x16,0x19,0x11,0x11,0x11},{0x04,0,0x0C,0x04,0x04,0x04,0x0E},
    {0x02,0,0x06,0x02,0x02,0x12,0x0C},{0x10,0x10,0x12,0x14,0x18,0x14,0x12},
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},{0,0,0x1A,0x15,0x15,0x11,0x11},
    {0,0,0x16,0x19,0x11,0x11,0x11},{0,0,0x0E,0x11,0x11,0x11,0x0E},
    {0,0x1E,0x11,0x11,0x1E,0x10,0x10},{0,0x0D,0x13,0x11,0x0F,0x01,0x01},
    {0,0,0x16,0x19,0x10,0x10,0x10},{0,0,0x0F,0x10,0x0E,0x01,0x1E},
    {0x08,0x1E,0x08,0x08,0x08,0x09,0x06},{0,0,0x11,0x11,0x11,0x13,0x0D},
    {0,0,0x11,0x11,0x0A,0x0A,0x04},{0,0,0x11,0x15,0x15,0x1B,0x11},
    {0,0,0x11,0x0A,0x04,0x0A,0x11},{0,0,0x11,0x11,0x0F,0x01,0x0E},
    {0,0,0x1F,0x02,0x04,0x08,0x1F},{0x02,0x04,0x04,0x08,0x04,0x04,0x02},
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04},{0x08,0x04,0x04,0x02,0x04,0x04,0x08},
    {0x08,0x15,0x02,0,0,0,0}
};

// ---------------- stb_image (PNG) ----------------
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR   // avoids pow(); no need to link -lm
#include "stb_image.h"

// ---------------- Layout & Config ----------------
typedef struct { int x,y,w,h; uint8_t r,g,b,a; } Overlay;

typedef struct {
    char *text;
    int x,y;
    uint8_t r,g,b,a;
    int scale;

    // Per-text overrides (-1 = inherit global)
    int orient_override;        // -1 inherit, 0 portrait, 1 landscape
    int landscape_ccw_override; // -1 inherit, 0 CW, 1 CCW
    int flip_override;          // -1 inherit, 0 no, 1 yes
} TextItem;

typedef struct { char *path; int x,y; int alpha; float scale; } ImgLayer;

typedef enum { ORIENT_PORTRAIT=0, ORIENT_LANDSCAPE=1 } UiOrient;

typedef struct {
    // Files & rendering
    char background_png[512];
    int  background_flip;       // 0|1

    // Background placement
    int bg_x_mode;  // 0 = numeric, 1 = center
    int bg_y_mode;  // 0 = numeric, 1 = center
    int bg_x;       // used when mode == numeric
    int bg_y;       // used when mode == numeric

    // Global UI orientation
    UiOrient text_orient;       // portrait|landscape
    int  text_flip;             // 0|1
    int  text_landscape_ccw;    // 0=CW, 1=CCW (direction for "landscape")

    // Big framebuffer & viewport
    int fb_scale_percent;       // >=100
    int viewport_x;             // -1 => center
    int viewport_y;             // -1 => center

    // Streaming
    int fps;
    int once;
    int iface;

    // Layers
    Overlay  *overlays; int n_overlays;
    TextItem *texts;    int n_texts;
    ImgLayer *imgs;     int n_imgs;

    // Debug
    int debug;
} Layout;

// Metrics for token expansion
typedef struct {
    int have_temp;
    float temp_c;

    int have_usage;
    float usage_pct;

    // For delta calculation
    uint64_t prev_idle, prev_total;
    int prev_valid;
} Metrics;

// ---- fwd decl for helper used in parser
static int parse_bool_inherit(const char *v, int *out);

static void layout_init(Layout *L){
    memset(L,0,sizeof(*L));
    L->fps=0; L->once=1; L->iface=-1;
    L->background_flip=0;

    L->bg_x_mode=1; L->bg_y_mode=1; // center by default
    L->bg_x=0; L->bg_y=0;

    L->text_orient=ORIENT_PORTRAIT;
    L->text_flip=0;
    L->text_landscape_ccw=0; // CW by default

    L->fb_scale_percent=150;
    L->viewport_x=-1; L->viewport_y=-1;

    L->debug=0;
}

static UiOrient parse_orient(const char *v){
    if(v && strcasecmp(v,"landscape")==0) return ORIENT_LANDSCAPE;
    return ORIENT_PORTRAIT;
}

static void trim(char *s){
    int n=(int)strlen(s);
    while(n>0 && (s[n-1]=='\r'||s[n-1]=='\n'||isspace((unsigned char)s[n-1]))) s[--n]=0;
    int i=0; while(s[i] && isspace((unsigned char)s[i])) i++;
    if(i>0) memmove(s,s+i,strlen(s+i)+1);
}

static int parse_rect(const char *s, int *x,int *y,int *w,int *h){
    int X=0,Y=0,Wd=0,Ht=0;
    int n = sscanf(s," %d , %d , %d , %d ",&X,&Y,&Wd,&Ht);
    if(n!=4) return -1;
    *x=X; *y=Y; *w=Wd; *h=Ht; return 0;
}

static int parse_rgbA(const char *s, uint8_t *r,uint8_t *g,uint8_t *b,uint8_t *a){
    int R=0,G=0,B=0,A=255;
    int n = sscanf(s," %d , %d , %d , %d ",&R,&G,&B,&A);
    if (n < 3) return -1;
    if (R<0||R>255||G<0||G>255||B<0||B>255) return -1;
    if (n == 4) {
        if (A<0 || A>255) return -1;
        *a = (uint8_t)A;
    } else {
        *a = 255;
    }
    *r=(uint8_t)R; *g=(uint8_t)G; *b=(uint8_t)B;
    return 0;
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
    cur_text.scale=1; cur_text.a=255;
    cur_text.orient_override=-1;
    cur_text.landscape_ccw_override=-1;
    cur_text.flip_override=-1;

    ImgLayer cur_img;  memset(&cur_img,0,sizeof(cur_img));
    cur_img.alpha=255; cur_img.scale=1.0f;

    int have_text=0, have_img=0;

    char line[1024];
    while(fgets(line,sizeof line,f)){
        trim(line);
        if(line[0]==0||line[0]=='#') continue;
        if(line[0]=='['){
            if(sec==SEC_TEXT && have_text && cur_text.text){
                add_text(L,cur_text);
                memset(&cur_text,0,sizeof cur_text);
                cur_text.scale=1; cur_text.a=255;
                cur_text.orient_override=-1;
                cur_text.landscape_ccw_override=-1;
                cur_text.flip_override=-1;
                have_text=0;
            }
            if(sec==SEC_IMAGE && have_img && cur_img.path){
                add_img(L,cur_img);
                memset(&cur_img,0,sizeof cur_img);
                cur_img.alpha=255; cur_img.scale=1.0f;
                have_img=0;
            }
            if(!strcmp(line,"[overlay]")) sec=SEC_OVERLAY;
            else if(!strcmp(line,"[text]")) sec=SEC_TEXT;
            else if(!strcmp(line,"[image]")) sec=SEC_IMAGE;
            else sec=SEC_NONE;
            continue;
        }
        char *eq=strchr(line,'='); if(!eq) continue; *eq=0;
        char *k=line, *v=eq+1; trim(k); trim(v);

        if(sec==SEC_NONE){
            if(!strcmp(k,"background_png")) { strncpy(L->background_png,v,sizeof(L->background_png)-1); }
            else if(!strcmp(k,"background_flip")) L->background_flip=atoi(v);

            else if(!strcmp(k,"background_x")){
                if (strcasecmp(v,"center")==0) { L->bg_x_mode = 1; }
                else { L->bg_x_mode = 0; L->bg_x = atoi(v); }
            }
            else if(!strcmp(k,"background_y")){
                if (strcasecmp(v,"center")==0) { L->bg_y_mode = 1; }
                else { L->bg_y_mode = 0; L->bg_y = atoi(v); }
            }

            else if(!strcmp(k,"text_orientation")) L->text_orient=parse_orient(v);
            else if(!strcmp(k,"text_flip")) L->text_flip=atoi(v);
            else if(!strcmp(k,"text_landscape_dir")) L->text_landscape_ccw = (strcasecmp(v,"ccw")==0) ? 1 : 0;

            else if(!strcmp(k,"fb_scale_percent")) L->fb_scale_percent=atoi(v);
            else if(!strcmp(k,"viewport_x")) L->viewport_x = (strcasecmp(v,"center")==0)? -1 : atoi(v);
            else if(!strcmp(k,"viewport_y")) L->viewport_y = (strcasecmp(v,"center")==0)? -1 : atoi(v);

            else if(!strcmp(k,"fps")) L->fps=atoi(v);
            else if(!strcmp(k,"once")) L->once=atoi(v);
            else if(!strcmp(k,"iface")) L->iface=atoi(v);
            else if(!strcmp(k,"debug")) L->debug=atoi(v);

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
            else if(!strcmp(k,"scale")) cur_text.scale=atoi(v);
            else if(!strcmp(k,"color")){
                if(parse_rgbA(v,&cur_text.r,&cur_text.g,&cur_text.b,&cur_text.a)!=0) fprintf(stderr,"Bad text color\n");
            }
            else if(!strcmp(k,"orientation")) {
                if      (strcasecmp(v,"inherit")==0)   cur_text.orient_override = -1;
                else if (strcasecmp(v,"landscape")==0) cur_text.orient_override =  1;
                else if (strcasecmp(v,"portrait")==0)  cur_text.orient_override =  0;
                else fprintf(stderr,"[text] orientation must be portrait|landscape|inherit (got '%s')\n", v);
            }
            else if(!strcmp(k,"landscape_dir")) {
                if      (strcasecmp(v,"inherit")==0) cur_text.landscape_ccw_override = -1;
                else if (strcasecmp(v,"ccw")==0)     cur_text.landscape_ccw_override =  1;
                else if (strcasecmp(v,"cw")==0)      cur_text.landscape_ccw_override =  0;
                else fprintf(stderr,"[text] landscape_dir must be cw|ccw|inherit (got '%s')\n", v);
            }
            else if(!strcmp(k,"flip")) {
                int tmp;
                if (parse_bool_inherit(v, &tmp)==0) cur_text.flip_override = tmp;
                else fprintf(stderr,"[text] flip must be 0|1|true|false|yes|no|inherit (got '%s')\n", v);
            }
        } else if(sec==SEC_IMAGE){
            have_img=1;
            if(!strcmp(k,"path")){ if(cur_img.path) free(cur_img.path); cur_img.path=strdup(v); }
            else if(!strcmp(k,"x")) cur_img.x=atoi(v);
            else if(!strcmp(k,"y")) cur_img.y=atoi(v);
            else if(!strcmp(k,"alpha")) cur_img.alpha=atoi(v);
            else if(!strcmp(k,"scale")) cur_img.scale=(float)atof(v);
        }
    }
    if(sec==SEC_TEXT && have_text && cur_text.text){ add_text(L,cur_text); }
    else if(cur_text.text){ free(cur_text.text); }
    if(sec==SEC_IMAGE && have_img && cur_img.path){ add_img(L,cur_img); }
    else if(cur_img.path){ free(cur_img.path); }
    fclose(f);

    if(L->background_png[0]==0){
        fprintf(stderr,"layout.cfg missing 'background_png='\n");
        return -1;
    }
    if(L->fb_scale_percent < 100) L->fb_scale_percent = 100;
    return 0;
}

// ---------------- Token helpers (%CPU_TEMP%, %CPU_USAGE%) ----------------
static int parse_bool_inherit(const char *v, int *out) {
    if (!v) return -1;
    if (strcasecmp(v, "inherit") == 0) { *out = -1; return 0; }
    if (!strcasecmp(v,"0")||!strcasecmp(v,"false")||!strcasecmp(v,"no")) { *out=0; return 0; }
    if (!strcasecmp(v,"1")||!strcasecmp(v,"true")||!strcasecmp(v,"yes")) { *out=1; return 0; }
    char *end=NULL; long n=strtol(v,&end,10);
    if (end && *end==0) { *out = (n!=0); return 0; }
    return -1;
}

static int read_file_ll(const char *path, long long *out){
    FILE *f=fopen(path,"r"); if(!f) return -1;
    char buf[64];
    if(!fgets(buf,sizeof buf,f)){ fclose(f); return -1; }
    fclose(f);
    char *end=NULL; errno=0;
    long long v=strtoll(buf,&end,10);
    if(errno!=0) return -1;
    *out=v; return 0;
}

// Try to get CPU temperature (°C). Strategy:
// - Look at /sys/class/thermal/thermal_zone*/temp : pick the highest positive temp
// - Fallback: /sys/class/hwmon/hwmon*/temp*_input : pick highest
static int get_cpu_temp_c(float *out_c){
    long long best = -1;

    // thermal_zone
    for(int i=0;i<32;i++){
        char p[128];
        snprintf(p,sizeof p,"/sys/class/thermal/thermal_zone%d/temp", i);
        long long v;
        if(read_file_ll(p,&v)==0){
            if (v > 1000) v = (v + 5) / 10; // convert to deci-degree to keep precision
            if (v > best) best = v;
        }
    }

    // hwmon (often millidegree C)
    for(int h=0; h<16; ++h){
        for(int t=1; t<=8; ++t){
            char p[160];
            snprintf(p,sizeof p,"/sys/class/hwmon/hwmon%d/temp%d_input",h,t);
            long long v;
            if(read_file_ll(p,&v)==0){
                if (v > 1000) v = v/100; // to deci-degree again
                if (v > best) best = v;
            }
        }
    }

    if(best < 0) return -1;
    // best is in deci-degC; convert to C with one decimal
    *out_c = best / 10.0f;
    return 0;
}

// Read /proc/stat and compute usage percentage from previous snapshot
static int read_cpu_totals(uint64_t *idle, uint64_t *total){
    FILE *f=fopen("/proc/stat","r"); if(!f) return -1;
    char line[256];
    if(!fgets(line,sizeof line,f)){ fclose(f); return -1; }
    fclose(f);
    // line starts with: cpu  user nice system idle iowait irq softirq steal guest guest_nice
    unsigned long long u=0, n=0, s=0, i=0, w=0, irq=0, sirq=0, st=0;
    int count = sscanf(line,"cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &u,&n,&s,&i,&w,&irq,&sirq,&st);
    if(count < 4) return -1;
    *idle = i + w;
    *total = u + n + s + *idle + irq + sirq + st;
    return 0;
}

static void metrics_init(Metrics *m){ memset(m,0,sizeof *m); }

// blocking_initial: if no previous sample and we're only rendering once, we
// take a short 60ms pause to get a delta for CPU usage.
static void update_metrics(Metrics *m, int blocking_initial){
    // Temp
    float tc;
    if(get_cpu_temp_c(&tc)==0){ m->have_temp=1; m->temp_c=tc; }

    // CPU usage
    uint64_t idle=0,total=0;
    if(read_cpu_totals(&idle,&total)==0){
        if(!m->prev_valid){
            if(blocking_initial){
                struct timespec ts={0, 60*1000*1000}; // 60ms
                nanosleep(&ts,NULL);
                uint64_t idle2=0,total2=0;
                if(read_cpu_totals(&idle2,&total2)==0){
                    uint64_t didle= (idle2 - idle);
                    uint64_t dtotal=(total2 - total);
                    if(dtotal>0){
                        float used = (float)(dtotal - didle) * 100.0f / (float)dtotal;
                        if(used < 0) used = 0; if(used > 100) used = 100;
                        m->usage_pct=used; m->have_usage=1;
                    }
                    m->prev_idle=idle2; m->prev_total=total2; m->prev_valid=1;
                }
            } else {
                m->prev_idle=idle; m->prev_total=total; m->prev_valid=1;
            }
        } else {
            uint64_t didle = idle - m->prev_idle;
            uint64_t dtotal= total - m->prev_total;
            if(dtotal>0){
                float used = (float)(dtotal - didle) * 100.0f / (float)dtotal;
                if(used < 0) used = 0; if(used > 100) used = 100;
                m->usage_pct=used; m->have_usage=1;
            }
            m->prev_idle=idle; m->prev_total=total;
        }
    }
}

// Expand tokens in a text line using the provided metrics.
// Supported: %CPU_TEMP%, %CPU_USAGE%
static void expand_tokens(char *out, size_t outsz, const char *in, const Metrics *m){
    size_t oi=0;
    for (size_t i=0; in[i] && oi+1<outsz; ){
        if(in[i]=='%'){
            const char *start = in + i + 1;
            const char *end = strchr(start,'%');
            if(end){
                size_t len = (size_t)(end - start);
                char tok[64]={0};
                if(len < sizeof(tok)){
                    for(size_t k=0;k<len;k++){
                        char c = start[k];
                        tok[k] = (char)((c>='a'&&c<='z')? (c-32) : c);
                    }
                    tok[len]=0;
                    char repl[64]={0};
                    int replaced=0;
                    if(strcmp(tok,"CPU_TEMP")==0){
                        if(m && m->have_temp){
                            // Round to 0 decimal if there's .0; else 1 decimal
                            // We'll print like "42°C" (no space)
                            int tenths = (int)(m->temp_c*10.0f + 0.5f);
                            int whole = tenths/10, dec = tenths%10;
                            if(dec==0) snprintf(repl,sizeof repl,"%d°C", whole);
                            else snprintf(repl,sizeof repl,"%d.%d°C", whole,dec);
                        }else{
                            snprintf(repl,sizeof repl,"N/A");
                        }
                        replaced=1;
                    } else if(strcmp(tok,"CPU_USAGE")==0){
                        if(m && m->have_usage){
                            int pct = (int)(m->usage_pct + 0.5f);
                            if(pct<0) pct=0; if(pct>100) pct=100;
                            snprintf(repl,sizeof repl,"%d%%", pct);
                        } else {
                            snprintf(repl,sizeof repl,"N/A");
                        }
                        replaced=1;
                    }
                    if(replaced){
                        size_t rl=strlen(repl);
                        for(size_t r=0; r<rl && oi+1<outsz; ++r) out[oi++]=repl[r];
                        i += (len + 2); // skip %TOKEN%
                        continue;
                    }
                }
                // unknown token -> copy literally the leading '%' and advance one
            }
        }
        out[oi++] = in[i++];
    }
    out[oi]=0;
}

// ---------------- PNG / RGBA helpers ----------------
typedef struct { int w,h,stride; uint8_t *rgba; } Png;

static Png load_png_rgba(const char *path){
    Png p={0}; int n=0;
    unsigned char *data = stbi_load(path,&p.w,&p.h,&n,4);
    if(!data){ fprintf(stderr,"stbi_load failed: %s\n", path); exit(1); }
    p.rgba=data; p.stride=p.w*4; return p;
}
static void free_png(Png *p){ if(p->rgba){ stbi_image_free(p->rgba); p->rgba=NULL; } }

static void rotate180_rgba(uint8_t *buf, int w, int h){
    size_t px=(size_t)w*h;
    for(size_t i=0,j=px-1;i<j;++i,--j){
        uint8_t t0=buf[i*4+0], t1=buf[i*4+1], t2=buf[i*4+2], t3=buf[i*4+3];
        buf[i*4+0]=buf[j*4+0]; buf[i*4+1]=buf[j*4+1]; buf[i*4+2]=buf[j*4+2]; buf[i*4+3]=buf[j*4+3];
        buf[j*4+0]=t0; buf[j*4+1]=t1; buf[j*4+2]=t2; buf[j*4+3]=t3;
    }
}

static void premultiply_rgba(uint8_t *px, int w, int h){
    size_t n=(size_t)w*h;
    for(size_t i=0;i<n;i++){
        uint8_t *p = px + i*4; uint16_t a=p[3];
        p[0]=(uint8_t)((p[0]*a + 127)/255);
        p[1]=(uint8_t)((p[1]*a + 127)/255);
        p[2]=(uint8_t)((p[2]*a + 127)/255);
    }
}

// dst and src premultiplied
static inline void over_premul(uint8_t *dst, const uint8_t *src){
    uint32_t aS=src[3]; uint32_t inv=255-aS;
    dst[0]=(uint8_t)(src[0] + (dst[0]*inv + 127)/255);
    dst[1]=(uint8_t)(src[1] + (dst[1]*inv + 127)/255);
    dst[2]=(uint8_t)(src[2] + (dst[2]*inv + 127)/255);
    dst[3]=(uint8_t)(aS + (dst[3]*inv + 127)/255);
}

static uint8_t* fb_rgba_alloc_clear(int fbw, int fbh){
    uint8_t *fb=(uint8_t*)calloc((size_t)fbw*fbh,4);
    if(!fb) die("calloc fb");
    return fb;
}

static inline void color_to_premul(uint8_t r,uint8_t g,uint8_t b,uint8_t a,uint8_t out[4]){
    out[0]=(uint8_t)((r*a + 127)/255);
    out[1]=(uint8_t)((g*a + 127)/255);
    out[2]=(uint8_t)((b*a + 127)/255);
    out[3]=a;
}

static void blit_png_into_fb(uint8_t *fb,int fbw,int fbh,const Png *png,int dstx,int dsty,int alpha,float scale){
    if(scale<=0.0f) scale=1.0f;
    int outw=(int)(png->w * scale);
    int outh=(int)(png->h * scale);
    for(int y=0;y<outh;y++){
        int sy=(int)((y/scale)+0.5f); if(sy<0)sy=0; if(sy>=png->h)sy=png->h-1;
        for(int x=0;x<outw;x++){
            int sx=(int)((x/scale)+0.5f); if(sx<0)sx=0; if(sx>=png->w)sx=png->w-1;
            int dx=dstx+x, dy=dsty+y;
            if((unsigned)dx>=(unsigned)fbw || (unsigned)dy>=(unsigned)fbh) continue;
            const uint8_t *s = png->rgba + 4*(sy*png->w + sx);
            uint8_t spx[4] = { s[0],s[1],s[2],s[3] };
            if(alpha>=0 && alpha<255){
                spx[0]=(uint8_t)((spx[0]*alpha + 127)/255);
                spx[1]=(uint8_t)((spx[1]*alpha + 127)/255);
                spx[2]=(uint8_t)((spx[2]*alpha + 127)/255);
                spx[3]=(uint8_t)((spx[3]*alpha + 127)/255);
            }
            over_premul(fb + 4*(dy*fbw + dx), spx);
        }
    }
}

// ---------------- UI Mapping (portrait/landscape + flip) ----------------
typedef enum { MAP_OK=0 } MapStatus;

static inline void map_ui_xy_fb(int xL,int yL,
                                UiOrient o,int flip180,
                                int *dx,int *dy,
                                int fbw,int fbh,
                                const Layout *L)
{
    int mx, my;

    if (o == ORIENT_PORTRAIT) {
        mx = xL;
        my = yL;
    } else {
        if (L->text_landscape_ccw) {
            // 90° CCW
            mx = W - 1 - yL;
            my = xL;
        } else {
            // 90° CW
            mx = yL;
            my = H - 1 - xL;
        }
    }

    if (flip180) {
        mx = W - 1 - mx;
        my = H - 1 - my;
    }

    int ox=(fbw - W)/2;
    int oy=(fbh - H)/2;

    *dx = ox + mx;
    *dy = oy + my;
}

static inline void put_px_ui(uint8_t *fb,int fbw,int fbh,
                             int xL,int yL,UiOrient o,int flip180,
                             uint8_t r,uint8_t g,uint8_t b,uint8_t a,
                             const Layout *L)
{
    int dx, dy;
    map_ui_xy_fb(xL,yL,o,flip180,&dx,&dy,fbw,fbh,L);
    if ((unsigned)dx >= (unsigned)fbw || (unsigned)dy >= (unsigned)fbh) return;
    uint8_t p[4];
    p[3] = a;
    p[0] = (uint8_t)((r*a + 127)/255);
    p[1] = (uint8_t)((g*a + 127)/255);
    p[2] = (uint8_t)((b*a + 127)/255);
    over_premul(fb + 4*(dy*fbw + dx), p);
}

static void draw_char_mapped(uint8_t *fb,int fbw,int fbh,int xL,int yL,char ch,
                             uint8_t r,uint8_t g,uint8_t b,uint8_t a,int scale,
                             UiOrient o,int flip180,const Layout *L)
{
    if (ch<32 || ch>126) return;
    const uint8_t *rows = font5x7[ch-32];
    int s = (scale<=0)?1:scale;

    for (int gy=0; gy<7; ++gy) {
        uint8_t row = rows[gy];
        for (int gx=0; gx<5; ++gx) {
            if (row & (1<<(4-gx))) {
                for (int sy=0; sy<s; ++sy)
                    for (int sx=0; sx<s; ++sx)
                        put_px_ui(fb,fbw,fbh,
                                  xL + gx*s + sx, yL + gy*s + sy,
                                  o, flip180, r,g,b,a, L);
            }
        }
    }
}

static void draw_text_mapped(uint8_t *fb,int fbw,int fbh,const TextItem *ti,
                             UiOrient global_o,int global_flip,const Layout *L,
                             const Metrics *M)
{
    // Resolve effective settings (apply per-text overrides if present)
    UiOrient o = global_o;
    int flip = global_flip;
    int ccw = L->text_landscape_ccw;

    if (ti->orient_override != -1)
        o = (ti->orient_override == 1) ? ORIENT_LANDSCAPE : ORIENT_PORTRAIT;
    if (ti->flip_override != -1)
        flip = ti->flip_override;
    if (ti->landscape_ccw_override != -1)
        ccw = ti->landscape_ccw_override;

    Layout L_local = *L;
    L_local.text_landscape_ccw = ccw;

    // Expand tokens into a local buffer
    char textbuf[1024];
    expand_tokens(textbuf, sizeof textbuf, ti->text ? ti->text : "", M);

    int s = (ti->scale<=0)?1:ti->scale;
    int x = ti->x, y = ti->y;

    for (const char *p=textbuf; *p; ++p) {
        if (*p=='\n') { y += 8*s; x = ti->x; continue; }
        draw_char_mapped(fb,fbw,fbh, x,y, *p,
                         ti->r,ti->g,ti->b,ti->a, s,
                         o, flip, &L_local);
        x += 6*s;
    }
}

static void draw_overlay_ui(uint8_t *fb,int fbw,int fbh,Overlay ov,
                            UiOrient o,int flip180,const Layout *L)
{
    int LW=(o==ORIENT_PORTRAIT)? W : H;
    int LH=(o==ORIENT_PORTRAIT)? H : W;
    int x0=ov.x<0?0:ov.x;
    int y0=ov.y<0?0:ov.y;
    int x1=ov.x+ov.w; if(x1>LW) x1=LW;
    int y1=ov.y+ov.h; if(y1>LH) y1=LH;

    for(int y=y0;y<y1;y++){
        for(int x=x0;x<x1;x++){
            put_px_ui(fb,fbw,fbh, x,y, o,flip180, ov.r,ov.g,ov.b,ov.a, L);
        }
    }
}

// ---------------- Viewport & RGB565 ----------------
static int FBWg=(W*3)/2, FBHg=(H*3)/2;

static void compute_fb_and_view(const Layout *L){
    int p=(L->fb_scale_percent<100)?100:L->fb_scale_percent;
    FBWg = (W * p + 99)/100;
    FBHg = (H * p + 99)/100;
}

static void compute_viewport(const Layout *L, int *vx, int *vy){
    int x = (L->viewport_x < 0) ? (FBWg - W)/2 : L->viewport_x;
    int y = (L->viewport_y < 0) ? (FBHg - H)/2 : L->viewport_y;

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (x > FBWg - W) x = FBWg - W;
    if (y > FBHg - H) y = FBHg - H;

    *vx = x;
    *vy = y;
}

static uint8_t* viewport_to_rgb565(const uint8_t *fb,int fbw,int fbh,int vx,int vy){
    (void)fbh;
    size_t px=(size_t)W*H;
    uint8_t *out=(uint8_t*)malloc(px*2);
    if(!out) die("malloc565");
    size_t j=0;
    for(int y=0;y<H;y++){
        const uint8_t *row = fb + 4*((vy+y)*fbw + vx);
        for(int x=0;x<W;x++){
            uint8_t pr=row[0], pg=row[1], pb=row[2], pa=row[3];
            uint8_t r,g,b;
            if(pa==0){ r=g=b=0; }
            else if(pa==255){ r=pr; g=pg; b=pb; }
            else {
                r=(uint8_t)((pr*255 + (pa>>1))/pa);
                g=(uint8_t)((pg*255 + (pa>>1))/pa);
                b=(uint8_t)((pb*255 + (pa>>1))/pa);
            }
            uint16_t v=((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3);
            out[j++]=(uint8_t)(v & 0xFF);
            out[j++]=(uint8_t)(v >> 8);
            row+=4;
        }
    }
    return out;
}

// ---------------- USB Robust Sender ----------------
static void build_header_fixed(uint8_t hdr[PACK]){
    // Header pulled from usb using wireshark (H=240, W=320).
    memset(hdr,0,PACK);
    hdr[0]=0xDA; hdr[1]=0xDB; hdr[2]=0xDC; hdr[3]=0xDD; // magic
    hdr[4]=0x02; hdr[5]=0x00;   // ver=2
    hdr[6]=0x01; hdr[7]=0x00;   // cmd=1
    hdr[8]=0xF0; hdr[9]=0x00;   // H=240
    hdr[10]=0x40; hdr[11]=0x01; // W=320
    hdr[12]=0x02; hdr[13]=0x00; // fmt=2 (RGB565)
    hdr[22]=0x00; hdr[23]=0x58; hdr[24]=0x02; hdr[25]=0x00; // frame_len = 0x00025800
    hdr[26]=0x00; hdr[27]=0x00; hdr[28]=0x00; hdr[29]=0x08; // extra - no clue but I'm sure it does stuff'
}

static void ctrl_nudge(libusb_device_handle *h, uint16_t wIndex){
    (void)h; (void)wIndex; // reserved (no-op)
}

// Discovery & recovery
static int pick_iface_and_out_ep(libusb_device_handle *h,int want_iface,int *iface,unsigned char *ep_out){
    libusb_device* dev=libusb_get_device(h);
    struct libusb_config_descriptor* cfg=NULL;
    int rc=libusb_get_active_config_descriptor(dev,&cfg);
    if(rc) return rc;
    *iface=-1; *ep_out=0;
    for(int i=0;i<cfg->bNumInterfaces;i++){
        if(want_iface!=-1 && i!=want_iface) continue;
        const struct libusb_interface *itf=&cfg->interface[i];
        for(int a=0;a<itf->num_altsetting;a++){
            const struct libusb_interface_descriptor *alt=&itf->altsetting[a];
            for(int e=0;e<alt->bNumEndpoints;e++){
                const struct libusb_endpoint_descriptor *ep=&alt->endpoint[e];
                uint8_t type=ep->bmAttributes & 0x3;
                uint8_t addr=ep->bEndpointAddress;
                int is_out=((addr & 0x80)==0);
                if(is_out && type==LIBUSB_TRANSFER_TYPE_INTERRUPT){ *iface=alt->bInterfaceNumber; *ep_out=addr; goto done; }
                if(is_out && type==LIBUSB_TRANSFER_TYPE_BULK && *ep_out==0){ *iface=alt->bInterfaceNumber; *ep_out=addr; }
            }
        }
    }
done:
    libusb_free_config_descriptor(cfg);
    return (*iface<0 || *ep_out==0)? LIBUSB_ERROR_OTHER : 0;
}
static void ensure_claim(libusb_device_handle *h,int iface){
    if(libusb_kernel_driver_active(h,iface)==1) libusb_detach_kernel_driver(h,iface);
    int rc=libusb_claim_interface(h,iface);
    if(rc){ fprintf(stderr,"claim interface %d failed (%d)\n",iface,rc); exit(1); }
}
static void usb_soft_recover(libusb_device_handle *h,unsigned char ep_out){ libusb_clear_halt(h,ep_out); }
static int usb_reset_and_reclaim(libusb_device_handle *h,int *iface,unsigned char *ep_out,int want_iface){
    int rc=libusb_reset_device(h); if(rc) return rc;
    usleep(300*1000);
    int i; unsigned char e;
    rc=pick_iface_and_out_ep(h,want_iface,&i,&e); if(rc) return rc;
    *iface=i; *ep_out=e; ensure_claim(h,*iface); return 0;
}
static int usb_full_reopen(libusb_context **pctx,libusb_device_handle **ph,int want_iface,int *iface,unsigned char *ep_out){
    if(*ph){ libusb_close(*ph); *ph=NULL; }
    if(*pctx){ libusb_exit(*pctx); *pctx=NULL; }
    int rc=libusb_init(pctx); if(rc) return rc;
    for(int tries=0; tries<10; ++tries){
        *ph=libusb_open_device_with_vid_pid(*pctx,VID,PID);
        if(*ph){
            libusb_set_auto_detach_kernel_driver(*ph,1);
            rc=pick_iface_and_out_ep(*ph,want_iface,iface,ep_out);
            if(!rc){ ensure_claim(*ph,*iface); return 0; }
            libusb_close(*ph); *ph=NULL;
        }
        usleep(200*1000);
    }
    return LIBUSB_ERROR_NO_DEVICE;
}
static int out512_retry(libusb_context **pctx,libusb_device_handle **ph,int want_iface,int *iface,unsigned char *ep_out,const uint8_t *buf,int len){
    unsigned char pkt[PACK]; memset(pkt,0,sizeof pkt);
    if(len>PACK) len=PACK;
    memcpy(pkt,buf,len);
    for(int attempt=0; attempt<4; ++attempt){
        int xfer=0;
        int r=libusb_interrupt_transfer(*ph,*ep_out,pkt,PACK,&xfer,CL_TIMEOUT);
        if(r==LIBUSB_ERROR_PIPE || r==LIBUSB_ERROR_TIMEOUT)
            r=libusb_bulk_transfer(*ph,*ep_out,pkt,PACK,&xfer,CL_TIMEOUT);
        if(r==0 && xfer==PACK) return 0;
        if(attempt==0){ usb_soft_recover(*ph,*ep_out); usleep(50*1000); }
        else if(attempt==1){ (void)usb_reset_and_reclaim(*ph,iface,ep_out,want_iface); usleep(150*1000); }
        else { int rc=usb_full_reopen(pctx,ph,want_iface,iface,ep_out); if(rc) return rc; }
    }
    return LIBUSB_ERROR_IO;
}

// ---------------- Main ----------------
int main(void){
    Layout L;
    if(load_layout("layout.cfg",&L)!=0){
        fprintf(stderr,"Failed to load layout.cfg\n");
        return 1;
    }

    // 1) Big framebuffer
    compute_fb_and_view(&L);
    int fbw=FBWg, fbh=FBHg;

    // 2) USB open (so we can loop quickly with fps)
    libusb_context* ctx=NULL; libusb_device_handle* h=NULL;
    if(libusb_init(&ctx)){ fprintf(stderr,"libusb_init failed\n"); return 1; }
    h=libusb_open_device_with_vid_pid(ctx,VID,PID);
    if(!h){ fprintf(stderr,"device %04x:%04x not found\n",VID,PID); libusb_exit(ctx); return 1; }
    libusb_set_auto_detach_kernel_driver(h,1);

    int iface=-1; unsigned char ep_out=0;
    if(pick_iface_and_out_ep(h,L.iface,&iface,&ep_out)!=0){
        fprintf(stderr,"No OUT endpoint%s\n",(L.iface!=-1?" on requested iface":""));
        libusb_close(h); libusb_exit(ctx); return 1;
    }
    ensure_claim(h,iface);

    uint16_t wIndex=(uint16_t)iface;
    uint8_t hdr[PACK]; build_header_fixed(hdr);

    int period_ms=(L.fps>0)?(1000/L.fps):0;

    Metrics M; metrics_init(&M);
    int frame_idx=0;

    do{
        // Allocate/clear big FB per frame
        uint8_t *fb = fb_rgba_alloc_clear(fbw, fbh);

        // Update metrics (blocking small sample on first frame if we're sending only once)
        update_metrics(&M, (frame_idx==0 && period_ms==0));

        // Background
        Png bg = load_png_rgba(L.background_png);
        premultiply_rgba(bg.rgba,bg.w,bg.h);
        if(L.background_flip) rotate180_rgba(bg.rgba,bg.w,bg.h);

        int bgx = L.bg_x_mode ? (fbw - bg.w)/2 : L.bg_x;
        int bgy = L.bg_y_mode ? (fbh - bg.h)/2 : L.bg_y;
        if (bgx < -bg.w) bgx = -bg.w;
        if (bgy < -bg.h) bgy = -bg.h;
        if (bgx > fbw)   bgx = fbw;
        if (bgy > fbh)   bgy = fbh;
        blit_png_into_fb(fb,fbw,fbh,&bg,bgx,bgy,-1,1.0f);
        free_png(&bg);

        // Image layers (FB coords)
        for(int i=0;i<L.n_imgs;i++){
            Png im=load_png_rgba(L.imgs[i].path);
            premultiply_rgba(im.rgba,im.w,im.h);
            blit_png_into_fb(fb,fbw,fbh,&im, L.imgs[i].x, L.imgs[i].y,
                             L.imgs[i].alpha, (L.imgs[i].scale>0?L.imgs[i].scale:1.0f));
            free_png(&im);
        }

        // Overlays (UI logical coords -> FB)
        for(int i=0;i<L.n_overlays;i++)
            draw_overlay_ui(fb,fbw,fbh,L.overlays[i],L.text_orient,L.text_flip,&L);

        // Text (UI logical coords -> FB) with per-text overrides & token expansion
        for(int i=0;i<L.n_texts;i++)
            draw_text_mapped(fb,fbw,fbh,&L.texts[i],L.text_orient,L.text_flip,&L,&M);

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

        if(period_ms>0){
            struct timespec ts; ts.tv_sec=period_ms/1000; ts.tv_nsec=(long)(period_ms%1000)*1000000L;
            nanosleep(&ts,NULL);
        }
        frame_idx++;
    } while(period_ms>0 && L.once==0);

tx_done:
    if(h && iface>=0) libusb_release_interface(h,iface);
    if(h) libusb_close(h);
    if(ctx) libusb_exit(ctx);

    // free allocations
    for(int i=0;i<L.n_texts;i++) free(L.texts[i].text);
    for(int i=0;i<L.n_imgs;i++) free(L.imgs[i].path);
    free(L.texts); free(L.overlays); free(L.imgs);
    puts("Frame sent.");
    return 0;
}
