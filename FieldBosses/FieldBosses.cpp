#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "../NativeScene.h"
#include "FieldBossRules.h"
#include "NativeBossGlow.h"
#include <vector>
#include <algorithm>
#include <cstring>
namespace fieldmod {
static bool enabled=true;static int slots=3,cap=3;static ULONGLONG nextCheck=0;
static cube::World* current=nullptr;static cube::Creature* sessionPlayer=nullptr;static int sessionSeed=0;static uint64_t worldKey=0;
struct Location{int type;unsigned char ocean,pad[3];int subtype,radius,x,y;float distanceSquared,influence;};
struct Active{cube::Creature* creature;int64_t id;IntVector2 region;unsigned slot;unsigned flags;short toggles;float graphics,hitbox;char oldName[16];};
static std::vector<Active> active;
static uintptr_t base(){return (uintptr_t)GetModuleHandleW(nullptr);}
static float maxHP(cube::Creature* c){return ((float(*)(cube::Creature*))(base()+0x5fac0))(c);}
// Floor conversion is required for negative world coordinates.
static int64_t block(int64_t dots){auto q=dots/65536;return dots<0&&dots%65536?q-1:q;}
static IntVector2 owner(cube::World*w,fieldboss::Site p){IntVector2 r{};((void(*)(cube::World*,IntVector2*,int,int))(base()+0x2ada20))(w,&r,(int)p.x,(int)p.y);return r;}
static Location location(cube::World*w,fieldboss::Site p){Location l{};((void(*)(cube::World*,int,int,Location*))(base()+0x35ce0))(w,(int)p.x,(int)p.y,&l);return l;}
static std::wstring section(IntVector2 r,unsigned slot){wchar_t out[128];swprintf_s(out,L"W%016llX_R%d_%d_S%u",worldKey,r.x,r.y,slot);return out;}
static int state(IntVector2 r,unsigned slot,const wchar_t* key,int fallback){return GetPrivateProfileIntW(section(r,slot).c_str(),key,fallback,L".\\Mods\\FieldBossesState.ini");}
static void save(IntVector2 r,unsigned slot,const wchar_t*key,int value){wchar_t out[32];swprintf_s(out,L"%d",value);WritePrivateProfileStringW(section(r,slot).c_str(),key,out,L".\\Mods\\FieldBossesState.ini");}
static cube::Creature* resident(cube::World*w,const Active&a){for(auto c:w->creatures)if(c==a.creature&&c->id==a.id)return c;return nullptr;}
static void restore(cube::Creature*c,const Active&a){auto&e=c->entity_data;float before=maxHP(c);float fraction=before>0?std::clamp(e.HP/before,0.f,1.f):1;e.appearance.flags2=a.flags;e.binary_toggles=a.toggles;e.appearance.graphics_scale=a.graphics;e.appearance.hitbox_scale=a.hitbox;memcpy(e.name,a.oldName,16);e.HP=maxHP(c)*fraction;}
static void initialize(){if(!nativeScene::supported()){enabled=false;return;}auto setting=[](const wchar_t*k,int v){return (int)GetPrivateProfileIntW(L"FieldBosses",k,v,L".\\Mods\\FieldBosses.ini");};enabled=setting(L"Enabled",1)!=0;slots=std::clamp(setting(L"BossesPerRegion",3),1,6);cap=std::clamp(setting(L"ActiveCap",3),1,6);if(enabled){bool glow=fieldglow::initialize();FILE*f=nullptr;fopen_s(&f,"Mods\\FieldBosses.log","a");if(f){fprintf(f,"Named-boss purple glow hook: %s\n",glow?"installed":"unavailable (native light preserved)");fclose(f);}}}
static void tick(cube::Game*g){
 if(!enabled)return;
 if(!g||!g->host.running||!g->world||!g->world->local_creature){active.clear();sessionPlayer=nullptr;current=nullptr;return;}
 auto w=&g->host.world;auto player=g->world->local_creature;
 if(current!=w||sessionPlayer!=player||sessionSeed!=w->seed){active.clear();current=w;sessionPlayer=player;sessionSeed=w->seed;nextCheck=0;worldKey=(uint32_t)w->seed;for(unsigned char ch:w->world_name)worldKey=(worldKey^ch)*1099511628211ull;}
 auto now=GetTickCount64();if(now<nextCheck)return;nextCheck=now+5000;
 fieldboss::Site playerPos{block(player->entity_data.position.x),block(player->entity_data.position.y)};
 for(auto it=active.begin();it!=active.end();){auto c=resident(w,*it);if(!c){it=active.erase(it);continue;}if(c->entity_data.HP<=0){save(it->region,it->slot,L"Defeated",1);it=active.erase(it);continue;}
  fieldboss::Site p{block(c->entity_data.position.x),block(c->entity_data.position.y)};
  if(!fieldboss::withinRadius(p,playerPos,256)){restore(c,*it);it=active.erase(it);continue;}++it;
 }
 if((int)active.size()>=cap)return;
 auto region=owner(g->world,playerPos);
 for(unsigned slot=0;slot<(unsigned)slots;slot++){
  if(state(region,slot,L"Defeated",0))continue;
  if(std::find_if(active.begin(),active.end(),[&](const Active&a){return a.region==region&&a.slot==slot;})!=active.end())continue;
  auto site=fieldboss::site(region.x,region.y,worldKey,slot);if(!fieldboss::withinRadius(site,playerPos,192))continue;
  int savedRace=state(region,slot,L"Race",-1);cube::Creature* choice=nullptr;uint64_t best=UINT64_MAX;
  for(auto c:w->creatures){
   if(!c||c==player)continue;auto&e=c->entity_data;
   if(e.hostility_type!=1||!fieldboss::nativeId(c->id)||(e.appearance.flags2&(fieldboss::Named|fieldboss::Boss|fieldboss::Mini|fieldboss::Friendly))||e.HP<=0||e.current_ability||e.stun_time>0)continue;
   if(savedRace!=-1&&e.race!=(unsigned)savedRace)continue;
   fieldboss::Site p{block(e.position.x),block(e.position.y)};if(!fieldboss::withinRadius(p,site,96)||!fieldboss::withinRadius(p,playerPos,192)||fieldboss::withinRadius(p,playerPos,32))continue;
   if(!(owner(g->world,p)==region))continue;
   auto l=location(g->world,p);if(l.type&&l.influence>0)continue; // Field encounters avoid named POIs.
   auto zc=IntVector2((int)std::floor(p.x/64.),(int)std::floor(p.y/64.));auto found=g->world->zones.find(zc);if(found==g->world->zones.end()||!found->second)continue;
   auto zone=found->second;int lx=(int)(p.x-(int64_t)zc.x*64),ly=(int)(p.y-(int64_t)zc.y*64),z=(int)block(e.position.z);
   cube::Block support{};((cube::Block*(*)(cube::Zone*,cube::Block*,int,int,int))(base()+0xeb90))(zone,&support,(int)p.x,(int)p.y,z);
   if(support.type==cube::Block::Water||support.type==cube::Block::Lava||support.type==cube::Block::Poison)continue;
   if(nativeScene::blockType(zone,lx,ly,z+2)!=cube::Block::Air||nativeScene::blockType(zone,lx,ly,z+3)!=cube::Block::Air)continue;
   if(!fieldboss::eligible(c->id,e.hostility_type,e.appearance.flags2,e.HP,maxHP(c),false))continue;
   uint64_t rank=fieldboss::hash(region.x,region.y,worldKey^(uint64_t)e.race,slot);
   // Species-first deterministic roll; creature ID breaks ties within species.
   rank^=(uint64_t)c->id&0xffff;if(rank<best){choice=c;best=rank;}
  }
  if(!choice)continue;auto&e=choice->entity_data;Active a{choice,choice->id,region,slot,e.appearance.flags2,e.binary_toggles,e.appearance.graphics_scale,e.appearance.hitbox_scale,{}};memcpy(a.oldName,e.name,16);
  e.appearance.flags2=fieldboss::bossFlags(e.appearance.flags2);e.appearance.graphics_scale*=1.25f;e.appearance.hitbox_scale*=1.25f;fieldboss::name(e.name,fieldboss::hash(region.x,region.y,worldKey,slot));e.HP=maxHP(choice);
  save(region,slot,L"Race",(int)e.race);active.push_back(a);return;
 }
}
}
class FieldBosses:public GenericMod{public:void Initialize()override{fieldmod::initialize();}void OnGameTick(cube::Game*g)override{fieldmod::tick(g);}void OnCreatureDeath(cube::Game*,cube::Creature*c,cube::Creature*)override{
 for(auto it=fieldmod::active.begin();it!=fieldmod::active.end();++it)if(it->creature==c&&it->id==c->id){fieldmod::save(it->region,it->slot,L"Defeated",1);fieldmod::active.erase(it);break;}
}};
static_assert(sizeof(GenericMod)==0x428);
EXPORT GenericMod*MakeMod(){return new FieldBosses();}EXPORT void ModPreInitialize(){}EXPORT int ModMajorVersion(){return 7;}EXPORT int ModMinorVersion(){return 3;}
