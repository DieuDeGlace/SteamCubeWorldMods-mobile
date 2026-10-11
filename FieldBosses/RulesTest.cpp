#include "NativeBossGlow.h"
#include <cassert>
#include <cstring>
static void capture(void* output,const fieldglow::Light* light){*(fieldglow::Light*)output=*light;}
int main(){
 for(int x=-100;x<=100;x++)for(int y=-5;y<=5;y++)for(unsigned slot=0;slot<6;slot++){
  auto a=fieldboss::site(x,y,42,slot),b=fieldboss::site(x,y,42,slot);assert(a.x==b.x&&a.y==b.y);assert(a.x>=(int64_t)x*1024+128&&a.x<(int64_t)x*1024+896);assert(a.y>=(int64_t)y*1024+128&&a.y<(int64_t)y*1024+896);
  char name[16];memset(name,255,16);fieldboss::name(name,fieldboss::hash(x,y,42,slot));assert(strlen(name)>0&&strlen(name)<16);
 }
 assert(fieldboss::eligible(1001,1,0,100,100,false));
 assert(fieldboss::eligible(-8737452766389660863ll,1,0,100,100,false));assert(!fieldboss::eligible(999,1,0,100,100,false));assert(!fieldboss::eligible(0,1,0,100,100,false));assert(!fieldboss::eligible(1001,3,0,100,100,false));assert(!fieldboss::eligible(1001,1,0,40,100,false));assert(!fieldboss::eligible(1001,1,fieldboss::Boss,100,100,false));assert(!fieldboss::eligible(1001,1,0,100,100,true));
 unsigned flags=fieldboss::bossFlags(fieldboss::Friendly|fieldboss::Mini|1);assert((flags&(fieldboss::Boss|fieldboss::Named))==(fieldboss::Boss|fieldboss::Named));assert(!(flags&(fieldboss::Friendly|fieldboss::Mini)));assert(flags&1);
 assert(fieldboss::withinRadius({-10000000,200},{-10000000,200},1));assert(!fieldboss::withinRadius({0,0},{300,0},256));
 float purple[3]{};fieldboss::purple(purple);assert(purple[0]==.6f&&purple[1]==.04f&&purple[2]==1.2f);
 fieldglow::Light original{{120,240,360},16,{.25f,.1f,.5f},420,0},output{};
 fieldglow::append=&capture;
 fieldglow::glow(&output,&original);
 assert(!memcmp(output.color,purple,sizeof(purple)));assert(original.color[0]==.25f);
 memcpy(output.color,original.color,sizeof(original.color));assert(!memcmp(&output,&original,sizeof(original)));
 fieldglow::glow(&output,&original);assert(!memcmp(output.color,purple,sizeof(purple)));
 puts("Native named-boss RGB remains consistent without tracked IDs; position/radius/sort key/input preserved: PASS");
 puts("Deterministic anchors/names, unsigned native IDs, hostile filtering, boss flags and purple RGB: PASS");
}
