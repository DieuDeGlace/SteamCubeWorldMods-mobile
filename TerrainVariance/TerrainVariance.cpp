#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "TerrainRules.h"
#include "../NativeScene.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace terrainmod {
static uintptr_t base;
static bool installed=false;
static double amplitude=68,scale=768,mountainAmplitude=80;
static int mountainLocationType=0;
static thread_local unsigned generating=0,recursion=0;
struct Location {int type;unsigned char ocean,pad[3];int subtype,radius,x,y;float distanceSquared,influence;};
using Height=float(*)(cube::World*,int,int,float,float,float,float,float,float,float,float,Location*,int);
using Generate=void(*)(cube::Zone*);
using Lookup=cube::Block*(*)(cube::Zone*,cube::Block*,int,int,int);
static Height originalHeight;
static Generate originalGenerate;
static Lookup originalLookup;
struct Columns {std::array<int8_t,4096> delta{};};
static std::unordered_map<cube::Zone*,std::shared_ptr<Columns>> columns;
static std::shared_mutex columnsMutex;
static std::atomic<uint64_t> epoch{1},chunks{0};
static std::atomic<uint64_t> details{0};
static void log(const char* s){FILE* f=nullptr;fopen_s(&f,"Mods\\TerrainVariance.log","a");if(f){fprintf(f,"%s\n",s);fclose(f);}}
static int fromSurface(cube::World* w,int64_t x,int64_t y,float native){
 if(!w||!std::isfinite(native)||x<INT_MIN||x>INT_MAX||y<INT_MIN||y>INT_MAX)return 0;
 // Use native biome ownership rather than temperature or the player's region.
 IntVector2 region{};
 ((void(*)(cube::World*,IntVector2*,int,int))(base+0x2ada20))(w,&region,(int)x,(int)y);
 int biome=((int(*)(cube::World*,int,int))(base+0x2b9080))(w,region.x,region.y);
 double weight=terrain::mountainRegionWeight(x,y,biome==(int)cube::Zone::RegionType::Mountains);
 double limit=amplitude+(mountainAmplitude-amplitude)*weight;
 double added=terrain::variation(x,y,(uint32_t)w->seed,limit,scale);
 Location location{};
 ((void(*)(cube::World*,int,int,Location*))(base+0x35ce0))(w,(int)x,(int)y,&location);
 if(mountainLocationType&&location.type==mountainLocationType&&location.influence>0)
  added=terrain::mountainRelief(added,mountainAmplitude,location.influence);
 return terrain::surfaceOffset(native,added);
}
static int displacement(cube::World* w,int64_t x,int64_t y){
 if(!w)return 0;
 // Special height modes share the normal ground mode's integer translation.
 // Temporarily suppress our height hook while obtaining that native baseline.
 struct Guard{Guard(){++generating;}~Guard(){--generating;}} guard;
 float native=((float(*)(cube::World*,int,int,int))(base+0x35ea0))(w,(int)x,(int)y,0);
 return fromSurface(w,x,y,native);
}
static const Columns* offsets(cube::Zone* zone){
 // Nine neighbors serve mesh sampling. Epoch changes prevent recycled zone
 // addresses from reusing stale offsets; shared ownership covers destruction.
 struct Entry {cube::Zone* zone=nullptr;std::shared_ptr<Columns> data;};
 static thread_local std::array<Entry,9> cache;
 static thread_local uint64_t seen=0;
 static thread_local unsigned next=0;
 static thread_local unsigned last=0;
 uint64_t current=epoch.load(std::memory_order_acquire);
 if(current!=seen){for(auto& e:cache){e.zone=nullptr;e.data.reset();}seen=current;}
 if(cache[last].zone==zone)return cache[last].data.get();
 for(unsigned i=0;i<cache.size();i++)if(cache[i].zone==zone){last=i;return cache[i].data.get();}
 std::shared_lock lock(columnsMutex);auto found=columns.find(zone);
 auto data=found==columns.end()?std::shared_ptr<Columns>{}:found->second;
 cache[next]={zone,data};last=next;next=(next+1)%cache.size();return cache[last].data.get();
}
static float height(cube::World* w,int x,int y,float a,float b,float c,float d,float e,float f,float g,float h,Location* l,int flags){
 if(generating||recursion)return originalHeight(w,x,y,a,b,c,d,e,f,g,h,l,flags);
 struct Guard{Guard(){++recursion;}~Guard(){--recursion;}} guard;
 float native=originalHeight(w,x,y,a,b,c,d,e,f,g,h,l,flags);
 return std::isfinite(native)?native+(flags==0?fromSurface(w,x,y,native):displacement(w,x,y)):native;
}
static void generate(cube::Zone* zone){
 struct Guard{Guard(){++generating;}~Guard(){--generating;}} guard;
 originalGenerate(zone); // Complete native water, objects and structures before translating.
}
static cube::Block* lookup(cube::Zone* zone,cube::Block* out,int x,int y,int z){
 if(generating||!zone)return originalLookup(zone,out,x,y,z);
 int64_t lx=(int64_t)x-(int64_t)zone->position.x*64,ly=(int64_t)y-(int64_t)zone->position.y*64;
 if(lx<0||lx>=64||ly<0||ly>=64)return originalLookup(zone,out,x,y,z);
 auto data=offsets(zone);if(!data)return originalLookup(zone,out,x,y,z);
 auto& field=zone->fields[lx*64+ly];int ceiling=data->delta[lx*64+ly];
 // Match the verified native getter, including solid filler and dry-air bit 2.
 // Implicit water uses this column's translated sea level instead of global zero.
 if(z<field.base_z){memcpy(out,(void*)(base+0x54dcb0),sizeof(*out));return out;}
 int64_t index=(int64_t)z-field.base_z;
 if(index>=(int64_t)field.blocks.size())memcpy(out,(void*)(base+(z>ceiling?0x54dcb8:0x54dcc0)),sizeof(*out));
 else{*out=field.blocks[(size_t)index];if(out->type==cube::Block::Air&&z<=ceiling&&!(out->breakable&2))memcpy(out,(void*)(base+0x54dcc0),sizeof(*out));}
 return out;
}
struct DetailArray {unsigned char* begin,*end,*capacity;};
struct Placement {size_t array,stride,position;};
static constexpr Placement placements[]={{0x18,0x40,8},{0x90,0x40,8},{0x30,0xe0,0xa0},{0x48,0xe0,0xa0},{0x60,0xe0,8},{0x78,0xe0,8}};
static size_t detailCount(const DetailArray& a,size_t stride=64){uintptr_t begin=(uintptr_t)a.begin,end=(uintptr_t)a.end,capacity=(uintptr_t)a.capacity;if(!begin||end<begin||capacity<end||(end-begin)%stride||(end-begin)>16*1024*1024)return 0;return (end-begin)/stride;}
static void translateDetails(cube::Zone* zone,const Columns& columns){
 // Sprite arrays use 64-byte records; spawn/object arrays use 224-byte
 // records with different position offsets. Derived position caches still
 // require auditing before this expanded translation is deployed.
 for(auto layout:placements){
  DetailArray array;memcpy(&array,(unsigned char*)zone+layout.array,sizeof(array));size_t count=detailCount(array,layout.stride);
  for(size_t i=0;i<count;i++){auto record=array.begin+i*layout.stride;int64_t p[3];memcpy(p,record+layout.position,sizeof(p));
   // Native spawn arrays contain empty records with a zero position.
   if(layout.position==0xa0&&!record[0]&&!p[0]&&!p[1]&&!p[2])continue;
   int64_t x=terrain::blockFromDots(p[0]),y=terrain::blockFromDots(p[1]);
   int64_t lx=x-(int64_t)zone->position.x*64,ly=y-(int64_t)zone->position.y*64;
   int delta=lx>=0&&lx<64&&ly>=0&&ly<64?columns.delta[lx*64+ly]:displacement(zone->world,x,y);
   if(p[2]<INT64_MIN+96*65536LL||p[2]>INT64_MAX-96*65536LL)continue;
   p[2]=terrain::shiftedDetailZ(p[2],delta);memcpy(record+layout.position+16,&p[2],sizeof(p[2]));details.fetch_add(1,std::memory_order_relaxed);
  }
 }
}
static void translate(cube::Zone* zone){
 if(!installed||!zone||!zone->world)return;
 auto data=std::make_shared<Columns>();
 for(int x=0;x<64;x++)for(int y=0;y<64;y++){
  auto& field=zone->fields[x*64+y];int delta=fromSurface(zone->world,(int64_t)zone->position.x*64+x,(int64_t)zone->position.y*64+y,field.field_8);
  data->delta[x*64+y]=(int8_t)delta;
  // Do not reallocate or rewrite the game's voxel storage. All materials move together.
  field.base_z+=delta;field.field_8+=delta;field.field_28+=delta;
 }
 translateDetails(zone,*data);
 {std::unique_lock lock(columnsMutex);columns[zone]=data;epoch.fetch_add(1,std::memory_order_release);}
 // Object occupancy is derived from object bounds, not part of the terrain.
 // Remove its old marks in their translated storage before the native builder
 // retires its old coordinate keys and marks the new bounds. Other bits survive.
 for(auto& field:zone->fields)for(auto& block:field.blocks)block.breakable&=~0x40;
 ((void(*)(cube::Zone*))(base+0x31d470))(zone); // collision bounds and copied effect origins
 ((void(*)(cube::Zone*))(base+0x31d700))(zone); // object occupancy and spatial keys
 auto n=++chunks;if(n==1||n%128==0){char s[256];sprintf_s(s,"Column alignment chunks=%llu, translated detail anchors=%llu; amplitude=%.0f scale=%.0f",(unsigned long long)n,(unsigned long long)details.load(),amplitude,scale);log(s);}
}
static void forget(cube::Zone* zone){std::unique_lock lock(columnsMutex);columns.erase(zone);epoch.fetch_add(1,std::memory_order_release);}
static bool matches(uintptr_t rva,const char* hex){for(size_t i=0;hex[i*2];i++){unsigned n=0;if(sscanf_s(hex+i*2,"%2x",&n)!=1||*(unsigned char*)(base+rva+i)!=n)return false;}return true;}
static void jump(unsigned char* out,uintptr_t target){out[0]=0xff;out[1]=0x25;memset(out+2,0,4);memcpy(out+6,&target,8);}
struct Hook {unsigned char* entry;unsigned size;void* replacement;unsigned char* trampoline=nullptr;DWORD protection=0;};
static bool prepare(Hook& h){
 // Nearby allocation permits relocating the lookup's RIP-relative cookie load.
 uintptr_t start=((uintptr_t)h.entry)&~uintptr_t(65535);
 for(uintptr_t d=65536;d<0x70000000;d+=65536){h.trampoline=(unsigned char*)VirtualAlloc((void*)(start+d),64,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(h.trampoline)break;}
 if(!h.trampoline)return false;memcpy(h.trampoline,h.entry,h.size);
 if(h.size==16){int32_t old;memcpy(&old,h.entry+9,4);int64_t relocated=(int64_t)(h.entry+13+old)-(int64_t)(h.trampoline+13);if(relocated<INT32_MIN||relocated>INT32_MAX)return false;int32_t value=(int32_t)relocated;memcpy(h.trampoline+9,&value,4);}
 jump(h.trampoline+h.size,(uintptr_t)h.entry+h.size);DWORD unused;
 return VirtualProtect(h.trampoline,64,PAGE_EXECUTE_READ,&unused)&&VirtualProtect(h.entry,h.size,PAGE_EXECUTE_READWRITE,&h.protection);
}
static void install(){
 base=(uintptr_t)GetModuleHandleW(nullptr);if(!nativeScene::supported()){log("Unsupported executable; terrain disabled.");return;}
 if(!GetPrivateProfileIntW(L"Terrain",L"Enabled",1,L".\\Mods\\TerrainVariance.ini")){log("TerrainVariance disabled.");return;}
 amplitude=std::clamp((double)GetPrivateProfileIntW(L"Terrain",L"Amplitude",68,L".\\Mods\\TerrainVariance.ini"),0.,96.);
 scale=std::clamp((double)GetPrivateProfileIntW(L"Terrain",L"Scale",768,L".\\Mods\\TerrainVariance.ini"),256.,2048.);
 mountainAmplitude=std::clamp((double)GetPrivateProfileIntW(L"Terrain",L"MountainAmplitude",80,L".\\Mods\\TerrainVariance.ini"),amplitude,96.);
 // Verified native name branch 16b454 resolves "Mountain". Read its jump
 // table index instead of assuming a location ID from branch order.
 for(int i=0;i<47;i++)if(*(uint32_t*)(base+0x16b7dc+i*4)==0x16b454)mountainLocationType=i+1;
 if(!mountainLocationType){log("Mountain location dispatch differs; terrain disabled.");return;}
 {char status[256];sprintf_s(status,"Mountain relief: ordinary amplitude=%.0f, mountain amplitude=%.0f; native location ID=%d; smooth 160-block region-border taper.",amplitude,mountainAmplitude,mountainLocationType);log(status);}
 if(!matches(0x2b4450,"4c8bdc5553415541564157498d6ba8")||!matches(0x2ff6f0,"40554154415541564157488dac24507dffff")||!matches(0xeb90,"40534883ec40488b05f3a053004833c4")){log("Column alignment: unsupported or hooked native code; disabled.");return;}
 if(!matches(0x31d470,"48895c242056574154415641574883ec40")||!matches(0x31d700,"40555356574156488d6c24c94881ece0000000")){log("Object cache builders differ; terrain disabled.");return;}
 Hook hooks[]={{(unsigned char*)(base+0x2b4450),15,(void*)height},{(unsigned char*)(base+0x2ff6f0),18,(void*)generate},{(unsigned char*)(base+0xeb90),16,(void*)lookup}};
 bool ready=true;for(auto& h:hooks)if(!prepare(h)){ready=false;break;}
 if(!ready){for(auto& h:hooks){if(h.protection){DWORD unused;VirtualProtect(h.entry,h.size,h.protection,&unused);}if(h.trampoline)VirtualFree(h.trampoline,0,MEM_RELEASE);}log("Column alignment: hook preparation failed; no patches applied.");return;}
 originalHeight=(Height)hooks[0].trampoline;originalGenerate=(Generate)hooks[1].trampoline;originalLookup=(Lookup)hooks[2].trampoline;
 for(auto& h:hooks){unsigned char patch[18];memset(patch,0x90,sizeof(patch));jump(patch,(uintptr_t)h.replacement);memcpy(h.entry,patch,h.size);DWORD unused;VirtualProtect(h.entry,h.size,h.protection,&unused);FlushInstructionCache(GetCurrentProcess(),h.entry,h.size);FlushInstructionCache(GetCurrentProcess(),h.trampoline,64);}
 installed=true;log("TerrainVariance column alignment enabled; native generation preserved, complete columns and water ceiling translated, all height modes uniform; API 7.3.");
}
static DWORD auditLookup(){
 // This export never installs hooks or changes a game's fields. Compare the
 // replacement at zero displacement against the actual native getter byte for byte.
 if(installed){log("Read-only block audit skipped: already installed.");return 0;}base=(uintptr_t)GetModuleHandleW(nullptr);
 if(!matches(0xeb90,"40534883ec40488b05f3a053004833c4")){log("Read-only block audit: native getter signature differs.");return 0;}
 originalLookup=(Lookup)(base+0xeb90);auto game=nativeScene::game();auto world=game?game->world:nullptr;
 nativeScene::Locks locks(world);if(!locks.held){log(world?"Read-only block audit: generation locks busy.":"Read-only block audit: no supported world.");return 0;}
 uint64_t checked=0,mismatches=0;unsigned sampled=0;
 for(auto& item:world->zones){auto zone=item.second;if(!zone)continue;
  {std::unique_lock lock(columnsMutex);columns[zone]=std::make_shared<Columns>();epoch.fetch_add(1,std::memory_order_release);}
  for(int x=0;x<64;x+=7)for(int y=0;y<64;y+=11){auto& f=zone->fields[x*64+y];
   int zs[]={f.base_z-1,f.base_z,f.base_z+(int)f.blocks.size()-1,f.base_z+(int)f.blocks.size(),-1,0,1};
   for(int z:zs){cube::Block expected{},actual{};int bx=zone->position.x*64+x,by=zone->position.y*64+y;
    originalLookup(zone,&expected,bx,by,z);lookup(zone,&actual,bx,by,z);checked++;if(memcmp(&actual,&expected,sizeof(actual)))mismatches++;
   }
  }
  forget(zone);if(++sampled==32)break;
 }
 char s[256];sprintf_s(s,"Read-only native block audit: zones=%u samples=%llu mismatches=%llu; no game fields modified.",sampled,(unsigned long long)checked,(unsigned long long)mismatches);log(s);
 return checked&&mismatches==0?1:0;
}
static DWORD auditDetails(){
 base=(uintptr_t)GetModuleHandleW(nullptr);auto g=nativeScene::game();auto world=g?g->world:nullptr;nativeScene::Locks locks(world);if(!locks.held){log("Read-only detail audit: generation locks busy or no world.");return 0;}
 uint64_t count=0,inZone=0;unsigned zones=0;double gap=0;std::map<std::pair<size_t,unsigned>,std::pair<uint64_t,uint64_t>> inventory;
 for(auto& item:world->zones){auto zone=item.second;if(!zone)continue;
  for(auto layout:placements){DetailArray array;memcpy(&array,(unsigned char*)zone+layout.array,sizeof(array));size_t n=detailCount(array,layout.stride);
   for(size_t i=0;i<n;i++){auto record=array.begin+i*layout.stride;unsigned id=0;memcpy(&id,record,layout.position==0xa0?1:4);auto& group=inventory[{layout.array,id}];group.first++;int64_t p[3];memcpy(p,record+layout.position,sizeof(p));int64_t x=terrain::blockFromDots(p[0])-(int64_t)zone->position.x*64,y=terrain::blockFromDots(p[1])-(int64_t)zone->position.y*64;count++;
    if(x>=0&&x<64&&y>=0&&y<64){inZone++;group.second++;gap=std::max(gap,std::abs(p[2]/65536.-zone->fields[x*64+y].field_8));}
   }
  }
  ++zones;
 }
 FILE* csv=nullptr;fopen_s(&csv,".mod-work\\TerrainVariance\\placement-id-audit.csv","w");if(csv){fprintf(csv,"array_offset,id,count,in_owner_zone\n");for(auto& item:inventory)fprintf(csv,"%zx,%u,%llu,%llu\n",item.first.first,item.first.second,(unsigned long long)item.second.first,(unsigned long long)item.second.second);fclose(csv);}
 char s[256];sprintf_s(s,"Read-only detail audit: zones=%u anchors=%llu in-owner-zone=%llu largest ground gap=%.3f; no positions modified.",zones,(unsigned long long)count,(unsigned long long)inZone,gap);log(s);return count&&inZone*100/count>=95?1:0;
}
static DWORD snapshotColumns(){
 auto g=nativeScene::game();auto w=g?g->world:nullptr;nativeScene::Locks locks(w);if(!locks.held||!w->local_creature)return 0;
 int64_t px=terrain::blockFromDots(w->local_creature->entity_data.position.x),py=terrain::blockFromDots(w->local_creature->entity_data.position.y);
 FILE* file=nullptr;fopen_s(&file,".mod-work\\TerrainVariance\\live-columns.csv","w");if(!file)return 0;
 fprintf(file,"x,y,height,delta,native_height,base_z,count,type_m2,type_m1,type_0,type_p1,type_p2\n");unsigned count=0;
 for(int dx=-256;dx<=256;dx+=4)for(int dy=-256;dy<=256;dy+=4){int64_t x=px+dx,y=py+dy;int zx=(int)std::floor(x/64.),zy=(int)std::floor(y/64.);auto found=w->zones.find(IntVector2(zx,zy));if(found==w->zones.end()||!found->second)continue;
  auto& f=found->second->fields[(x-(int64_t)zx*64)*64+y-(int64_t)zy*64];int delta=fromSurface(w,x,y,f.field_8);int z=(int)std::floor(f.field_8);
  fprintf(file,"%lld,%lld,%.6f,%d,%.6f,%d,%zu",x,y,f.field_8,delta,f.field_8-delta,f.base_z,f.blocks.size());
  for(int dz=-2;dz<=2;dz++){int64_t index=(int64_t)z+dz-f.base_z;int type=index<0?1:index>=(int64_t)f.blocks.size()?0:f.blocks[(size_t)index].type;fprintf(file,",%d",type);}fprintf(file,"\n");count++;
 }
 fclose(file);char s[160];sprintf_s(s,"Read-only local column snapshot: %u columns around (%lld,%lld); no terrain modified.",count,px,py);log(s);return count;
}
}
class TerrainVariance:public GenericMod{
public:
 TerrainVariance(){OnZoneGeneratedPriority=VeryLowPriority;}
 void Initialize()override{terrainmod::install();}
 void OnZoneGenerated(cube::Zone* zone)override{terrainmod::translate(zone);}
 void OnZoneDestroy(cube::Zone* zone)override{terrainmod::forget(zone);}
};
static_assert(sizeof(GenericMod)==0x428);
EXPORT GenericMod* MakeMod(){return new TerrainVariance();}
EXPORT void ModPreInitialize(){}
EXPORT int ModMajorVersion(){return 7;}
EXPORT int ModMinorVersion(){return 3;}
extern "C" __declspec(dllexport) DWORD WINAPI AuditColumnLookup(void*){return terrainmod::auditLookup();}
extern "C" __declspec(dllexport) DWORD WINAPI AuditDetailPositions(void*){for(unsigned i=0;i<100;i++){auto result=terrainmod::auditDetails();if(result)return result;Sleep(5);}return 0;}
extern "C" __declspec(dllexport) DWORD WINAPI SnapshotTerrainColumns(void*){for(unsigned i=0;i<100;i++){auto result=terrainmod::snapshotColumns();if(result)return result;Sleep(5);}return 0;}

