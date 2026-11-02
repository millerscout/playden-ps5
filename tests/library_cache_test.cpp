#include "library/cache.h"
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include "steam_id.h"
using namespace playden;
void Check(bool value,const char* message){if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main(){
 char root[]="/tmp/playden-cache-XXXXXX";Check(mkdtemp(root),"temporary root");
 Storage storage(root);LibraryCache cache(storage);
 LibrarySnapshot original{1,123456,{{SteamId(10),"Portal","https://example.invalid/10.jpg","Puzzle","Valve"},{SteamId(20),"星の旅","","Travel","Studio"}}};
 Check(cache.Save("76561198000000001",original).ok(),"save a real account snapshot");
 auto restored=cache.Load("76561198000000001");Check(restored.ok(),"load snapshot after creating a new cache reader");
 Check(restored.value().games.size()==2 && restored.value().games[1].title=="星の旅","Unicode title roundtrip");
 Check(restored.value().retrieved_at_unix_seconds==123456,"stale timestamp preserved");
 Check(!restored.value().games[0].playtime_minutes && !restored.value().games[0].last_played_unix_seconds,"old schema never invents history");
 auto with_history=original;with_history.schema_version=2;
 with_history.games[0].playtime_minutes=0;
 with_history.games[0].last_played_unix_seconds=123400;
 with_history.games[1].playtime_minutes=999;
 Check(cache.Save("76561198000000003",with_history).ok(),"save real history in version two");
 auto history=cache.Load("76561198000000003");
 Check(history.ok() && history.value().schema_version==2,"load version two");
 Check(history.value().games[0].playtime_minutes==0 && history.value().games[0].last_played_unix_seconds==123400,"zero playtime and timestamp roundtrip");
 Check(history.value().games[1].playtime_minutes==999 && !history.value().games[1].last_played_unix_seconds,"unknown last played remains unknown");
 auto lossy=with_history;lossy.schema_version=1;
 Check(!cache.Save("76561198000000003",lossy).ok(),"old schema cannot silently discard history");
 Check(!cache.Load("76561198000000002").ok(),"another account never receives first account data");
 Check(!cache.Save("../escape",original).ok(),"reject account path traversal");
 auto bad=original;bad.schema_version=99;Check(!cache.Save("76561198000000001",bad).ok(),"unsupported schema cannot replace valid cache");
 Check(cache.Load("76561198000000001").ok(),"rejected update preserves previous snapshot");
 bad=original;bad.games.push_back(bad.games.front());Check(!cache.Save("76561198000000001",bad).ok(),"duplicate identities rejected before persistence");
 auto bytes=storage.Read("library-76561198000000001.cache");Check(bytes.ok(),"read cache for corruption fixture");
 auto corrupt=bytes.value();corrupt[corrupt.size()/2]^=1;Check(storage.WriteAtomic("library-76561198000000001.cache",corrupt).ok(),"write corruption fixture");
 Check(!cache.Load("76561198000000001").ok(),"checksum detects damaged cache");
 Check(storage.WriteAtomic("library-76561198000000002.cache",bytes.value()).ok(),"copy fixture into different account namespace");
 Check(!cache.Load("76561198000000002").ok(),"embedded account rejects wrongly copied cache");
 auto truncated=bytes.value();truncated.resize(10);
 Check(storage.WriteAtomic("library-76561198000000001.cache",truncated).ok(),"write truncated fixture");
 Check(!cache.Load("76561198000000001").ok(),"truncated cache rejected");
 LibrarySnapshot large{1,123458,{}};
 for(unsigned i=1;i<=32;++i)large.games.push_back({SteamId(i),"Large game","",std::string(40000,'x'),"Studio"});
 Check(cache.Save("76561198000000001",large).ok(),"save snapshot exceeding preference read limit");
 Check(!storage.Read("library-76561198000000001.cache").ok(),"default storage limit remains bounded");
 auto large_restored=cache.Load("76561198000000001");
 Check(large_restored.ok() && large_restored.value().games.size()==32,"cache explicitly reads bounded larger snapshot");
 auto limited=storage.Read("library-76561198000000001.cache",1234);
 Check(!limited.ok() && limited.message().find("1234-byte limit")!=std::string::npos,"read failure reports actual caller bound");
 Check(!storage.Read("library-76561198000000001.cache",0).ok(),"zero read bound rejected");
 Check(!storage.Read("library-76561198000000001.cache",33*1024*1024).ok(),"excessive read bound rejected");
 LibrarySnapshot empty{1,123457,{}};Check(cache.Save("76561198000000001",empty).ok(),"empty library is legitimate cached state");
 Check(cache.Load("76561198000000001").value().games.empty(),"empty library survives restart");
 LibrarySnapshot partial{3,123459,{{SteamId(30),"Partial","","",""}},true};
 Check(cache.Save(std::uint64_t{76561198000000004ULL},partial).ok(),"save schema three metadata state through repository API");
 auto partial_restored=cache.Load(std::uint64_t{76561198000000004ULL});
 Check(partial_restored.ok()&&partial_restored.value().metadata_partial,"schema three preserves partial metadata state");
 auto lossy_partial=partial;lossy_partial.schema_version=2;
 Check(!cache.Save("76561198000000004",lossy_partial).ok(),"schema two cannot silently discard partial metadata state");
 LibrarySnapshot dated{4,123460,{{SteamId(40),"Dated","","",""},{SteamId(41),"Undated","","",""}}};
 dated.games[0].added_at_unix_seconds=1500000000;
 Check(cache.Save("76561198000000005",dated).ok(),"save schema four acquisition dates");
 auto dated_restored=cache.Load("76561198000000005");
 Check(dated_restored.ok()&&dated_restored.value().schema_version==4,"load schema four");
 Check(dated_restored.value().games[0].added_at_unix_seconds==1500000000u&&!dated_restored.value().games[1].added_at_unix_seconds,"acquisition date roundtrip; unknown stays unknown");
 auto lossy_dated=dated;lossy_dated.schema_version=3;
 Check(!cache.Save("76561198000000005",lossy_dated).ok(),"schema three cannot silently discard acquisition dates");
 LibrarySnapshot demos{6,123461,{{SteamId(60),"Synthetic Demo","","",""},{SteamId(61),"Full Game","","",""}}};
 demos.games[0].demo=true;demos.games[0].added_at_unix_seconds=1600000000;
 Check(cache.Save("76561198000000006",demos).ok(),"save schema six demos");
 auto demos_restored=cache.Load("76561198000000006");
 Check(demos_restored.ok()&&demos_restored.value().schema_version==6,"load schema six");
 Check(demos_restored.value().games[0].demo&&demos_restored.value().games[0].added_at_unix_seconds==1600000000u&&
     !demos_restored.value().games[1].demo,"demo mark roundtrip; games stay unmarked");
 auto lossy_demos=demos;lossy_demos.schema_version=5;
 Check(!cache.Save("76561198000000006",lossy_demos).ok(),"schema five cannot silently discard demos");
 Check(!dated_restored.value().games[0].demo,"older schemas hold no demos");
 LibrarySnapshot shared{7,123462,{{SteamId(70),"Synthetic Shared","","",""},{SteamId(71),"Owned Game","","",""},{SteamId(72),"Shared Demo","","",""}}};
 shared.games[0].family_shared=true;shared.games[0].added_at_unix_seconds=1650000000;
 shared.games[2].family_shared=true;shared.games[2].demo=true;
 Check(cache.Save("76561198000000007",shared).ok(),"save schema seven family-shared games");
 auto shared_restored=cache.Load("76561198000000007");
 Check(shared_restored.ok()&&shared_restored.value().schema_version==7,"load schema seven");
 Check(shared_restored.value().games[0].family_shared&&shared_restored.value().games[0].added_at_unix_seconds==1650000000u&&
     !shared_restored.value().games[0].demo&&!shared_restored.value().games[1].family_shared&&
     shared_restored.value().games[2].family_shared&&shared_restored.value().games[2].demo,"family mark roundtrip next to demos; owned games stay unmarked");
 auto lossy_shared=shared;lossy_shared.schema_version=6;
 Check(!cache.Save("76561198000000007",lossy_shared).ok(),"schema six cannot silently discard family sharing");
 Check(!demos_restored.value().games[0].family_shared&&!dated_restored.value().games[0].family_shared,"older schemas hold no family sharing");
 LibrarySnapshot tagged{8,123463,{{SteamId(80),"Tagged Game","","",""},{SteamId(81),"Plain","","",""}}};
 tagged.games[0].store_tags={"Roguelike","Pixel Graphics"};tagged.games[0].store_tags_known=true;
 Check(cache.Save("76561198000000008",tagged).ok(),"save schema eight store tags");
 auto tagged_restored=cache.Load("76561198000000008");
 Check(tagged_restored.ok()&&tagged_restored.value().schema_version==8,"load schema eight");
 Check(tagged_restored.value().games[0].store_tags_known&&tagged_restored.value().games[0].store_tags.size()==2&&
     tagged_restored.value().games[0].store_tags[0]=="Roguelike"&&!tagged_restored.value().games[1].store_tags_known,"store tag roundtrip");
 auto lossy_tags=tagged;lossy_tags.schema_version=7;
 Check(!cache.Save("76561198000000008",lossy_tags).ok(),"schema seven cannot silently discard store tags");
 Check(!shared_restored.value().games[0].store_tags_known,"older schemas hold no store tags");
 Check(!tagged_restored.value().games[0].free,"schema eight holds no free mark");
 LibrarySnapshot free_games{9,123464,{{SteamId(90),"Free Game","","",""},{SteamId(91),"Owned Game","","",""},{SteamId(92),"Shared","","",""}}};
 free_games.games[0].free=true;free_games.games[0].store_tags={"Free to Play"};free_games.games[0].store_tags_known=true;
 free_games.games[2].family_shared=true;
 Check(cache.Save("76561198000000009",free_games).ok(),"save schema nine free games");
 auto free_restored=cache.Load("76561198000000009");
 Check(free_restored.ok()&&free_restored.value().schema_version==9,"load schema nine");
 Check(free_restored.value().games[0].free&&free_restored.value().games[0].store_tags_known&&
     free_restored.value().games[0].store_tags.size()==1&&!free_restored.value().games[1].free&&
     free_restored.value().games[2].family_shared&&!free_restored.value().games[2].free,"free mark roundtrip beside tags and family sharing");
 auto lossy_free=free_games;lossy_free.schema_version=8;
 Check(!cache.Save("76561198000000009",lossy_free).ok(),"schema eight cannot silently discard free games");
 Check(!tagged_restored.value().games[0].free&&!shared_restored.value().games[0].free,"older schemas hold no free games");
 auto zero_dated=dated;zero_dated.games[0].added_at_unix_seconds=0;
 Check(!cache.Save("76561198000000005",zero_dated).ok(),"zero acquisition date rejected");
 auto future=partial;future.retrieved_at_unix_seconds=std::uint64_t{1}<<63;
 Check(!cache.Save("76561198000000004",future).ok(),"timestamp beyond signed presentation range rejected");
 Check(storage.WriteAtomic("library-76561198000000004.cache.tmp",Bytes{'s','t','a','l','e'}).ok(),"write interrupted replacement fixture");
 Check(cache.Clear(std::uint64_t{76561198000000004ULL}).ok(),"clear numeric account cache");
 auto cleared=cache.Load("76561198000000004");
 Check(!cleared.ok()&&cleared.error()==StorageError::kNotFound,"cleared cache stays absent");
 auto stale_temporary=storage.Read("library-76561198000000004.cache.tmp");
 Check(!stale_temporary.ok()&&stale_temporary.error()==StorageError::kNotFound,"clear removes interrupted replacement file");
 Check(cache.Clear("76561198000000004").ok(),"clearing an absent cache is idempotent");
 Check(!cache.Clear(std::uint64_t{0}).ok(),"zero account is rejected");
 LibrarySnapshot held{9,123465,{{SteamId(100),"Held","","",""},{SteamId(101),"Other","","",""}}};
 Check(cache.Save("76561198000000010",held).ok(),"save library for tag merges");
 Check(cache.MergeGameTags(76561198000000010ULL,{{SteamId(100),{"Roguelike"}}}).ok(),"merge tags that differ");
 const auto merged_path=std::string(root)+"/library-76561198000000010.cache";
 struct stat first_file{};
 Check(stat(merged_path.c_str(),&first_file)==0,"stat the cache after a tag change");
 Check(cache.MergeGameTags(76561198000000010ULL,{{SteamId(100),{"Roguelike"}}}).ok(),"merge the same tags again");
 struct stat second_file{};
 Check(stat(merged_path.c_str(),&second_file)==0,"stat the cache after an unchanged merge");
 Check(first_file.st_ino==second_file.st_ino,"unchanged tags do not write");
 auto merged_bytes=storage.Read("library-76561198000000010.cache");
 Check(merged_bytes.ok(),"read the merged cache before damaging it");
 auto damaged=merged_bytes.value();damaged[damaged.size()/2]^=1;
 Check(storage.WriteAtomic("library-76561198000000010.cache",damaged).ok(),"damage the file under the resident snapshot");
 Check(cache.MergeGameTags(76561198000000010ULL,{{SteamId(101),{"Indie"}}}).ok(),"a changed merge does not read the damaged file");
 LibraryCache reread(storage);
 auto both=reread.Load("76561198000000010");
 Check(both.ok()&&both.value().games.size()==2,"changed tags were written from memory");
 Check(both.value().games[0].store_tags_known&&both.value().games[0].store_tags.size()==1&&
     both.value().games[0].store_tags[0]=="Roguelike","first game keeps its tags");
 Check(both.value().games[1].store_tags_known&&both.value().games[1].store_tags.size()==1&&
     both.value().games[1].store_tags[0]=="Indie","second game gained tags without a reload");
 LibrarySnapshot refreshed{9,123466,{{SteamId(100),"Held renamed","","",""},{SteamId(102),"Newcomer","","",""}}};
 Check(cache.Save("76561198000000010",refreshed).ok(),"library refresh replaces the snapshot");
 LibraryCache after_refresh(storage);
 auto kept=after_refresh.Load("76561198000000010");
 Check(kept.ok()&&kept.value().games.size()==2,"refresh writes the new library");
 Check(kept.value().games[0].title=="Held renamed"&&kept.value().games[0].store_tags_known&&
     kept.value().games[0].store_tags.size()==1&&kept.value().games[0].store_tags[0]=="Roguelike",
     "refresh keeps tags for a game that remains");
 Check(kept.value().games[1].title=="Newcomer"&&!kept.value().games[1].store_tags_known,
     "a new game has no tags and the dropped game is gone");
 std::remove((std::string(root)+"/library-76561198000000001.cache").c_str());std::remove((std::string(root)+"/library-76561198000000002.cache").c_str());std::remove((std::string(root)+"/library-76561198000000003.cache").c_str());std::remove(merged_path.c_str());rmdir(root);
 puts("library_cache_test passed");
}
