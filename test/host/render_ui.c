/* Render app_ui to a PPM so it can be looked at.
 *
 *     make -C test/host preview && xdg-open /tmp/wx-ok.ppm
 *
 * Deliberately not a test. The band-seam tests prove the layout renders consistently;
 * they cannot tell you the night shading buries the traces, or that a units label is
 * clipped against the left edge, or that a superscript is missing from the font. All
 * three of those were found here, by looking, in the first ten minutes of having it.
 *
 * Modes: (none) | stale | none | prov -- PLAN 4c's five situations less STALE_OLD,
 * which differs from stale only in colour.
 */
#include "app_ui.h"
#include <stdio.h>
#include <string.h>
static const uint8_t PAL[8][3] = {
    {0,0,0},{255,255,255},{0,255,0},{0,0,255},{255,0,0},{255,255,0},{255,140,0},{255,255,255}};
int main(int argc,char**argv){
    static uint8_t px[(size_t)APP_GFX_WIDTH*APP_GFX_HEIGHT];
    app_gfx_surface_t s={.pixels=px,.y0=0,.rows=APP_GFX_HEIGHT};
    app_wx_forecast_t f={0}; f.count=13; f.utc_offset_s=3600; f.fetched_at=1000;
    float t[13]={19.9f,18.3f,18.4f,18.2f,18.0f,18.3f,18.2f,17.8f,17.5f,18.1f,18.2f,18.2f,18.1f};
    float fl[13]={17.4f,15.6f,15.8f,15.5f,15.2f,15.6f,15.5f,14.9f,14.6f,15.3f,15.5f,15.4f,15.3f};
    float p[13]={0.1f,1.4f,0.9f,0,0,0,0,0,0,0,0,0.1f,0.7f};
    uint8_t pc[13]={20,80,70,10,5,5,5,5,5,10,15,30,60};
    int16_t sw[13]={66,0,0,0,0,0,0,0,0,0,0,0,13};
    int day[13]={1,0,0,0,0,0,0,0,0,0,0,0,1};
    for(int i=0;i<13;i++){f.hour[i].t=1000+3600*i;f.hour[i].temp_c=t[i];f.hour[i].feels_c=fl[i];
        f.hour[i].precip_mm=p[i];
        f.hour[i].precip_pct=pc[i];f.hour[i].solar_wm2=sw[i];f.hour[i].is_day=day[i];}
    f.day_count=2;
    f.day[0].sunrise=6*3600-3600;  f.day[0].sunset=20*3600-3600;
    f.day[1].sunrise=86400+6*3600-3600; f.day[1].sunset=86400+20*3600-3600;
    app_ui_state_t st={.mode=APP_UI_OK,.now=1000,.place="Dublin",.version="v0.1.0",
        .net_state="connected",.ip="192.168.1.124",.ap_ssid="",.ap_pass=""};
    if(argc>2&&!strcmp(argv[2],"stale")) st.mode=APP_UI_STALE, st.now=1000+45*60;
    if(argc>2&&!strcmp(argv[2],"none")) st.mode=APP_UI_NO_FORECAST;
    if(argc>2&&!strcmp(argv[2],"prov")) st.mode=APP_UI_UNPROVISIONED,
        st.ap_ssid="wx-station-a1b2c3", st.ap_pass="EXAMPLE7PASS";
    app_ui_draw(&s,&f,&st);
    FILE*o=fopen(argv[1],"wb");
    fprintf(o,"P6\n%d %d\n255\n",APP_GFX_WIDTH,APP_GFX_HEIGHT);
    for(size_t i=0;i<(size_t)APP_GFX_WIDTH*APP_GFX_HEIGHT;i++) fwrite(PAL[px[i]&7],1,3,o);
    fclose(o); return 0;
}
