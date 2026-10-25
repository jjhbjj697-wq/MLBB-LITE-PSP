#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

PSP_MODULE_INFO("MLBB Lite PSP", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define W 480
#define H 272
#define BUF_W 512
#define LIST_SIZE (256 * 1024)
#define HEROES 20
#define UNITS 10
#define PI 3.14159265f

static unsigned int __attribute__((aligned(16))) list[LIST_SIZE / 4];
static unsigned int __attribute__((aligned(16))) fb0[BUF_W * H];
static unsigned int __attribute__((aligned(16))) fb1[BUF_W * H];
static unsigned int __attribute__((aligned(16))) zbuf[BUF_W * H];

typedef struct { unsigned int color; float x, y; } Vertex;
typedef struct { const char *name; const char *role; unsigned int color; int hp, damage, range; } HeroDef;
typedef struct { float x, y; int hp, maxhp, team, hero, alive, cooldown, skillcool; } Unit;

/* Original primitive-color art only. No MLBB game assets are bundled. */
static const HeroDef roster[HEROES] = {
 {"Miya","Marksman",0xff55c8ff,100,10,34}, {"Layla","Marksman",0xff78a8ff,95,11,42},
 {"Tigreal","Tank",0xff8bd9ff,145,8,23}, {"Franco","Tank",0xff72d6e8,140,9,25},
 {"Eudora","Mage",0xffb879ff,85,14,31}, {"Nana","Mage/Support",0xffffa6e6,90,10,29},
 {"Zilong","Fighter",0xff79e6a0,115,12,24}, {"Alucard","Fighter",0xffd9b36a,120,13,25},
 {"Saber","Assassin",0xffd4e4f5,90,15,22}, {"Fanny","Assassin",0xff8fe8dd,95,13,26},
 {"Angela","Support",0xffff9fbc,95,7,25}, {"Rafaela","Support",0xffffdf91,100,7,27},
 {"Chou","Fighter",0xffffbd70,120,12,24}, {"Gusion","Assassin",0xffc4a0ff,90,14,26},
 {"Lesley","Marksman",0xffa6b7ff,90,13,40}, {"Balmond","Fighter",0xffff8a6f,135,11,23},
 {"Estes","Support",0xffb8e8a0,100,6,27}, {"Johnson","Tank",0xff92b8c8,150,8,22},
 {"Kagura","Mage",0xff9aa9ff,90,13,30}, {"Lancelot","Assassin",0xffff9f86,95,14,25}
};

static Unit units[UNITS];
static int selected = 0, selectedHero = 0, gameover = 0, winner = -1, frame = 0;
static int basehp[2] = {100,100};
static SceCtrlData oldpad;

static unsigned int rgb(int r,int g,int b) { return GU_RGBA(r,g,b,255); }
static void poly(int prim, Vertex *v, int n) {
    sceGuDrawArray(prim, GU_COLOR_8888 | GU_VERTEX_2F_FLOAT, n, 0, v);
}
static void rect(float x,float y,float w,float h,unsigned int c) {
    Vertex v[4]={{c,x,y},{c,x+w,y},{c,x+w,y+h},{c,x,y+h}};
    poly(GU_TRIANGLE_FAN,v,4);
}
static void line(float x1,float y1,float x2,float y2,unsigned int c) {
    Vertex v[2]={{c,x1,y1},{c,x2,y2}}; poly(GU_LINES,v,2);
}
static void circle(float x,float y,float r,unsigned int c) {
    Vertex v[11]; int i; v[0].color=c; v[0].x=x; v[0].y=y;
    for(i=0;i<=9;i++){float a=(float)i*2.0f*PI/9.0f;v[i+1].color=c;v[i+1].x=x+cosf(a)*r;v[i+1].y=y+sinf(a)*r;}
    poly(GU_TRIANGLE_FAN,v,11);
}
static float clampf(float v,float lo,float hi){return v<lo?lo:(v>hi?hi:v);}
static float dist2(float x1,float y1,float x2,float y2){float x=x1-x2,y=y1-y2;return x*x+y*y;}
static void init_units(void) {
    int i;
    for(i=0;i<UNITS;i++){
        int team=i/5, h=(i*3 + (team?4:0))%HEROES;
        units[i].team=team; units[i].hero=h; units[i].maxhp=roster[h].hp;
        units[i].hp=units[i].maxhp; units[i].alive=1; units[i].cooldown=0; units[i].skillcool=0;
        units[i].x=team? (390.0f+(i%5)*5.0f):(90.0f-(i%5)*5.0f);
        units[i].y=93.0f+(i%5)*21.0f;
    }
    /* Player starts as Miya; remaining four allies and five opponents are bots. */
    units[0].hero=0; units[0].maxhp=roster[0].hp; units[0].hp=units[0].maxhp;
}
static int nearest_enemy(int idx,float maxrange) {
    int j,best=-1; float bestd=maxrange*maxrange;
    for(j=0;j<UNITS;j++) if(units[j].alive && units[j].team!=units[idx].team) {
        float d=dist2(units[idx].x,units[idx].y,units[j].x,units[j].y);
        if(d<bestd){bestd=d;best=j;}
    }
    return best;
}
static void damage_unit(int target,int amount) {
    if(target<0||target>=UNITS||!units[target].alive)return;
    units[target].hp-=amount;
    if(units[target].hp<=0){units[target].hp=0;units[target].alive=0;}
}
static void attack(int idx,int bonus,int rangebonus) {
    int target=nearest_enemy(idx,(float)(roster[units[idx].hero].range+rangebonus));
    if(target>=0) damage_unit(target,roster[units[idx].hero].damage+bonus);
    else {
        int enemybase=1-units[idx].team;
        basehp[enemybase]-=(bonus?8:3);
        if(basehp[enemybase]<0)basehp[enemybase]=0;
    }
}
static void update_game(SceCtrlData *pad) {
    int i; Unit *p=&units[selected];
    if(pad->Buttons & PSP_CTRL_START){gameover=1;winner=-2;return;}
    if((pad->Buttons&PSP_CTRL_LTRIGGER)&&!(oldpad.Buttons&PSP_CTRL_LTRIGGER)) selectedHero=(selectedHero+HEROES-1)%HEROES;
    if((pad->Buttons&PSP_CTRL_RTRIGGER)&&!(oldpad.Buttons&PSP_CTRL_RTRIGGER)) selectedHero=(selectedHero+1)%HEROES;
    units[0].hero=selectedHero; units[0].maxhp=roster[selectedHero].hp;
    if(units[0].hp>units[0].maxhp)units[0].hp=units[0].maxhp;
    selected=0; p=&units[0];
    if(p->alive){
        float speed=1.7f;
        if(pad->Buttons&PSP_CTRL_UP)p->y-=speed;
        if(pad->Buttons&PSP_CTRL_DOWN)p->y+=speed;
        if(pad->Buttons&PSP_CTRL_LEFT)p->x-=speed;
        if(pad->Buttons&PSP_CTRL_RIGHT)p->x+=speed;
        p->x=clampf(p->x,20,460);p->y=clampf(p->y,45,225);
        if(p->cooldown>0)p->cooldown--; if(p->skillcool>0)p->skillcool--;
        if((pad->Buttons&PSP_CTRL_CROSS)&&!(oldpad.Buttons&PSP_CTRL_CROSS)&&p->cooldown==0){attack(selected,0,0);p->cooldown=18;}
        if((pad->Buttons&PSP_CTRL_SQUARE)&&!(oldpad.Buttons&PSP_CTRL_SQUARE)&&p->skillcool==0){attack(selected,12,18);p->skillcool=100;}
        if((pad->Buttons&PSP_CTRL_TRIANGLE)&&!(oldpad.Buttons&PSP_CTRL_TRIANGLE)&&p->skillcool==0){
            int j; for(j=0;j<UNITS;j++)if(units[j].alive&&units[j].team!=p->team&&dist2(p->x,p->y,units[j].x,units[j].y)<65.0f*65.0f)damage_unit(j,18);
            p->skillcool=140;
        }
        if((pad->Buttons&PSP_CTRL_CIRCLE)&&!(oldpad.Buttons&PSP_CTRL_CIRCLE)&&p->skillcool==0){p->hp+=25;if(p->hp>p->maxhp)p->hp=p->maxhp;p->skillcool=160;}
    }
    for(i=0;i<UNITS;i++) if(i!=selected && units[i].alive){
        Unit *u=&units[i]; int target=nearest_enemy(i,(float)roster[u->hero].range+8.0f); int j;
        if(u->cooldown>0)u->cooldown--;
        if(target>=0){
            if(u->cooldown==0){damage_unit(target,roster[u->hero].damage);u->cooldown=25;}
        } else {
            float goalx=u->team?75.0f:405.0f, goaly=136.0f;
            float dx=goalx-u->x,dy=goaly-u->y,len=sqrtf(dx*dx+dy*dy);
            if(len>2.0f){u->x+=dx/len*0.42f;u->y+=dy/len*0.28f;}
            else {int eb=1-u->team;basehp[eb]-=1;if(basehp[eb]<0)basehp[eb]=0;}
        }
        /* Simple bot support behavior: a nearby support occasionally heals an ally. */
        if((u->hero==10||u->hero==11||u->hero==16)&&frame%100==0){
            for(j=u->team*5;j<u->team*5+5;j++)if(units[j].alive&&units[j].hp<units[j].maxhp){units[j].hp+=5;if(units[j].hp>units[j].maxhp)units[j].hp=units[j].maxhp;break;}
        }
    }
    if(basehp[0]<=0){gameover=1;winner=1;} if(basehp[1]<=0){gameover=1;winner=0;}
    frame++;
}
static void draw_map(void) {
    Vertex lane[4]={{rgb(77,91,72),55,65},{rgb(77,91,72),425,65},{rgb(77,91,72),455,215},{rgb(77,91,72),25,215}};
    poly(GU_TRIANGLE_FAN,lane,4);
    /* grass patches and a winding central lane create a light 2.5D perspective */
    rect(0,0,W,42,rgb(36,75,58)); rect(0,230,W,42,rgb(36,75,58));
    line(70,65,410,65,rgb(116,137,91));line(25,215,455,215,rgb(116,137,91));
    line(80,136,400,136,rgb(184,171,120));line(80,132,400,132,rgb(210,195,143));
    circle(240,136,32,rgb(88,110,77)); circle(240,136,22,rgb(113,132,83));
    /* Bases and two defensive towers per side */
    rect(20,112,28,48,rgb(65,151,236));rect(432,112,28,48,rgb(235,89,88));
    rect(76,105,13,24,rgb(86,177,255));rect(76,143,13,24,rgb(86,177,255));
    rect(391,105,13,24,rgb(255,132,114));rect(391,143,13,24,rgb(255,132,114));
}
static void draw_units(void) {
    int i;
    for(i=0;i<UNITS;i++){
        Unit *u=&units[i]; const HeroDef *h=&roster[u->hero];
        float scale=0.65f+u->y/600.0f; float r=7.0f*scale;
        unsigned int col=u->team?rgb(231,76,76):rgb(66,164,244);
        if(!u->alive){circle(u->x,u->y+2,4,rgb(70,70,70));continue;}
        circle(u->x,u->y+3,r+2,rgb(33,42,38)); circle(u->x,u->y,r,col); circle(u->x,u->y-1,r*0.57f,h->color);
        rect(u->x-8,u->y-r-7,16,2,rgb(45,45,45));
        rect(u->x-8,u->y-r-7,16.0f*((float)u->hp/(float)u->maxhp),2,u->team?rgb(255,125,112):rgb(104,245,148));
        if(i==0){line(u->x-10,u->y+r+3,u->x+10,u->y+r+3,rgb(255,245,140));}
    }
}
static void draw_hud(void) {
    int i; Unit *p=&units[selected];
    rect(0,0,W,28,rgb(20,28,38));
    /* icon-like status bars; text-free so all rendering remains on the GU framebuffer */
    rect(8,7,90,5,rgb(64,64,64));rect(8,7,90.0f*((float)basehp[0]/100.0f),5,rgb(75,170,255));
    rect(382,7,90,5,rgb(64,64,64));rect(382,7,90.0f*((float)basehp[1]/100.0f),5,rgb(255,90,90));
    rect(180,5,120,7,rgb(55,55,55));rect(180,5,120.0f*((float)p->hp/(float)p->maxhp),7,rgb(112,240,160));
    for(i=0;i<4;i++){unsigned int c=(i==0)?rgb(255,255,255):rgb(145,145,145);circle(188+i*34,253,8,c);}
    if(p->cooldown>0)rect(178,244,22.0f*((float)p->cooldown/18.0f),3,rgb(255,198,80));
}
static void draw_frame(void) {
    sceGuStart(GU_DIRECT,list);sceGuClearColor(rgb(27,56,44));sceGuClearDepth(0);sceGuClear(GU_COLOR_BUFFER_BIT|GU_DEPTH_BUFFER_BIT);
    draw_map();draw_units();draw_hud();sceGuFinish();sceGuSync(0,0);sceDisplayWaitVblankStart();sceGuSwapBuffers();
}
int main(void) {
    SceCtrlData pad; memset(&oldpad,0,sizeof(oldpad));
    sceCtrlSetSamplingCycle(0);sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    sceGuInit();sceGuStart(GU_DIRECT,list);
    sceGuDrawBuffer(GU_PSM_8888,(void*)0,BUF_W);sceGuDispBuffer(W,H,(void*)(BUF_W*H*4),BUF_W);
    sceGuDepthBuffer((void*)(BUF_W*H*8),BUF_W);sceGuOffset(2048-(W/2),2048-(H/2));sceGuViewport(2048,2048,W,H);
    sceGuDepthRange(65535,0);sceGuScissor(0,0,W,H);sceGuEnable(GU_SCISSOR_TEST);sceGuDisable(GU_DEPTH_TEST);
    sceGuShadeModel(GU_FLAT);sceGuClearColor(rgb(27,56,44));sceGuClear(GU_COLOR_BUFFER_BIT|GU_DEPTH_BUFFER_BIT);sceGuFinish();sceGuSync(0,0);
    sceDisplayWaitVblankStart();sceGuDisplay(GU_TRUE);init_units();
game_loop:
    while(!gameover){sceCtrlReadBufferPositive(&pad,1);update_game(&pad);draw_frame();oldpad=pad;}
    /* simple end screen using a clear color; START exits, X restarts */
    {
        int restart=0;
        while(1){sceCtrlReadBufferPositive(&pad,1);if(pad.Buttons&PSP_CTRL_START)break;if((pad.Buttons&PSP_CTRL_CROSS)&&!(oldpad.Buttons&PSP_CTRL_CROSS)){basehp[0]=basehp[1]=100;selected=0;selectedHero=0;gameover=0;winner=-1;frame=0;init_units();restart=1;break;}oldpad=pad;
            sceGuStart(GU_DIRECT,list);sceGuClearColor(winner==0?rgb(25,80,120):(winner==1?rgb(110,35,35):rgb(25,25,25)));sceGuClear(GU_COLOR_BUFFER_BIT|GU_DEPTH_BUFFER_BIT);sceGuFinish();sceGuSync(0,0);sceDisplayWaitVblankStart();sceGuSwapBuffers();}
        if(restart)goto game_loop;
    }
    sceGuDisplay(GU_FALSE);sceGuTerm();sceKernelExitGame();return 0;
}
