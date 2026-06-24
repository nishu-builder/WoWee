#include "core/godview_recording.hpp"

#include <catch_amalgamated.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using wowee::core::GodviewRecording;

namespace {

std::filesystem::path writeTempRecording(const std::string& name, const std::string& contents) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                ("wowee_" + name + "_" + std::to_string(stamp) + ".jsonl");
    std::ofstream out(path);
    REQUIRE(out.is_open());
    out << contents;
    return path;
}

std::filesystem::path writeContractRecording() {
    return writeTempRecording(
        "godview_contract",
        R"JSON({"t":1700000002000,"ms":2000,"map":0,"instance":0,"players":[{"guid":50,"name":"Alpha","level":11,"race":1,"class":1,"gender":1,"x":20.0,"y":10.0,"z":5.0,"o":1.5707964,"hp":50,"maxhp":100,"target":"0x34","combat":true}]}
{"t":1700000001000,"ms":1000,"map":0,"instance":0,"players":[{"guid":50,"name":"Alpha","level":10,"race":1,"class":1,"x":10.0,"y":0.0,"z":0.0,"o":0.0,"hp":100,"maxhp":100,"target":"0x33","combat":false}]}
{"t":1700000001000,"ms":1000,"map":1,"instance":0,"players":[{"guid":"cow:1","name":"Othermap","level":8,"race":2,"class":7,"gender":1,"x":100.0,"y":200.0,"z":30.0,"o":3.0,"hp":80,"maxhp":80,"target":0,"combat":false}]}
{"t":1700000002000,"ms":2000,"map":1,"instance":0,"players":[{"guid":"cow:1","name":"Othermap","level":8,"race":2,"class":7,"gender":1,"x":110.0,"y":210.0,"z":30.0,"o":3.4,"hp":80,"maxhp":80,"target":0,"combat":false}]}
)JSON");
}

} // namespace

TEST_CASE("GodviewRecording loads Coworld JSONL in server-ms order", "[godview][recording]") {
    auto path = writeContractRecording();

    GodviewRecording recording;
    std::string error;
    REQUIRE(recording.load(path.string(), error));
    REQUIRE(error.empty());

    REQUIRE(recording.snapshotCount() == 4);
    REQUIRE(recording.startMs() == 1000);
    REQUIRE(recording.endMs() == 2000);
    std::vector<uint32_t> expectedMaps{0, 1};
    REQUIRE(recording.mapIds() == expectedMaps);
    REQUIRE(recording.snapshotCountForMap(0) == 2);
    REQUIRE(recording.snapshotCountForMap(1) == 2);
    REQUIRE(recording.startMsForMap(0) == 1000);
    REQUIRE(recording.endMsForMap(0) == 2000);

    const auto* firstMap0 = recording.firstPlayerForMap(0);
    REQUIRE(firstMap0 != nullptr);
    REQUIRE(firstMap0->guid == 50);
    REQUIRE(firstMap0->rawGuid == "50");
    REQUIRE(firstMap0->name == "Alpha");
    REQUIRE(firstMap0->level == 10);
    REQUIRE(firstMap0->gender == 0);
    REQUIRE(firstMap0->targetGuid);
    REQUIRE(*firstMap0->targetGuid == 0x33);

    const auto* firstMap1 = recording.firstPlayerForMap(1);
    REQUIRE(firstMap1 != nullptr);
    REQUIRE(firstMap1->rawGuid == "cow:1");
    REQUIRE((firstMap1->guid & 0x8000000000000000ull) != 0);
    REQUIRE(firstMap1->gender == 1);
}

TEST_CASE("GodviewRecording interpolates only within the selected map", "[godview][recording]") {
    auto path = writeContractRecording();

    GodviewRecording recording;
    std::string error;
    REQUIRE(recording.load(path.string(), error));

    auto map0At1250 = recording.samplePlayers(1250.0, 0);
    REQUIRE(map0At1250.size() == 1);
    REQUIRE(map0At1250[0].player.guid == 50);
    REQUIRE(map0At1250[0].player.name == "Alpha");
    REQUIRE(map0At1250[0].player.x == Catch::Approx(12.5f));
    REQUIRE(map0At1250[0].player.y == Catch::Approx(2.5f));
    REQUIRE(map0At1250[0].player.z == Catch::Approx(1.25f));
    REQUIRE(map0At1250[0].player.orientation == Catch::Approx(0.3926991f));
    REQUIRE(map0At1250[0].player.hp == 88);
    REQUIRE(map0At1250[0].player.level == 10);
    REQUIRE(map0At1250[0].player.combat == false);
    REQUIRE(map0At1250[0].player.targetGuid);
    REQUIRE(*map0At1250[0].player.targetGuid == 0x33);
    REQUIRE(map0At1250[0].moving == true);

    auto map1At1250 = recording.samplePlayers(1250.0, 1);
    REQUIRE(map1At1250.size() == 1);
    REQUIRE(map1At1250[0].player.name == "Othermap");
    REQUIRE(map1At1250[0].player.x == Catch::Approx(102.5f));
    REQUIRE(map1At1250[0].player.y == Catch::Approx(202.5f));

    auto pair = recording.findSnapshotPair(1250.0, 1);
    REQUIRE(pair.valid);
    REQUIRE(recording.snapshots()[pair.prev].map == 1);
    REQUIRE(recording.snapshots()[pair.next].map == 1);
}

TEST_CASE("GodviewRecording finds target or combat event snapshots per map", "[godview][recording]") {
    auto path = writeContractRecording();

    GodviewRecording recording;
    std::string error;
    REQUIRE(recording.load(path.string(), error));

    auto firstMap0 = recording.findTargetOrCombatEventMs(999.0, 0, 1);
    REQUIRE(firstMap0);
    REQUIRE(*firstMap0 == 1000);

    auto nextMap0 = recording.findTargetOrCombatEventMs(1000.0, 0, 1);
    REQUIRE(nextMap0);
    REQUIRE(*nextMap0 == 2000);

    auto firstCombatMap0 = recording.findEventMs(999.0, 0, GodviewRecording::EventKind::Combat, 1);
    REQUIRE(firstCombatMap0);
    REQUIRE(*firstCombatMap0 == 2000);
    REQUIRE_FALSE(recording.findEventMs(999.0, 1, GodviewRecording::EventKind::Combat, 1));

    auto prevMap0 = recording.findTargetOrCombatEventMs(2000.0, 0, -1);
    REQUIRE(prevMap0);
    REQUIRE(*prevMap0 == 1000);

    REQUIRE_FALSE(recording.findTargetOrCombatEventMs(2000.0, 0, 1));
    REQUIRE_FALSE(recording.findTargetOrCombatEventMs(999.0, 1, 1));
}

TEST_CASE("GodviewRecording loads v2 raw GUID equipment and creatures", "[godview][recording]") {
    auto path = writeTempRecording(
        "godview_v2",
        R"JSON({"t":1700000000000,"schema":2,"ms":1000,"map":1,"instance":0,"players":[{"guid":50,"raw_guid":"0x10000000032","name":"Replayhunt","level":4,"race":2,"class":7,"gender":1,"display_id":51,"native_display_id":51,"mount_display_id":0,"x":-620.0,"y":-4252.0,"z":40.0,"o":1.0,"hp":80,"maxhp":100,"target":2955,"target_raw":"0x20000000b8b","combat":false,"equipment":[{"slot":15,"item_id":36,"display_id":5195,"inventory_type":13,"class":2,"subclass":7}]}],"creatures":[{"guid":2955,"raw_guid":"0x20000000b8b","entry":2955,"name":"Plainstrider","level":6,"rank":0,"type":1,"display_id":390,"native_display_id":390,"x":-625.0,"y":-4250.0,"z":40.0,"o":2.0,"hp":60,"maxhp":60,"target":0,"target_raw":0,"combat":false,"dead":false}]}
{"t":1700000000500,"schema":2,"ms":1500,"map":1,"instance":0,"players":[{"guid":50,"raw_guid":"0x10000000032","name":"Replayhunt","level":4,"race":2,"class":7,"gender":1,"display_id":51,"native_display_id":51,"mount_display_id":0,"x":-618.0,"y":-4250.0,"z":40.5,"o":1.4,"hp":78,"maxhp":100,"target":2955,"target_raw":"0x20000000b8b","combat":true,"equipment":[{"slot":15,"item_id":36,"display_id":5195,"inventory_type":13,"class":2,"subclass":7}]}],"creatures":[{"guid":2955,"raw_guid":"0x20000000b8b","entry":2955,"name":"Plainstrider","level":6,"rank":0,"type":1,"display_id":390,"native_display_id":390,"x":-623.0,"y":-4248.0,"z":40.0,"o":2.4,"hp":35,"maxhp":60,"target":50,"target_raw":"0x10000000032","combat":true,"dead":false}]}
)JSON");

    GodviewRecording recording;
    std::string error;
    REQUIRE(recording.load(path.string(), error));
    REQUIRE(error.empty());

    REQUIRE(recording.snapshotCountForMap(1) == 2);
    REQUIRE(recording.creatureCountForMap(1) == 1);
    REQUIRE(recording.snapshots()[0].schema == 2);

    const auto& player = recording.snapshots()[0].players.front();
    REQUIRE(player.guid == 0x10000000032ull);
    REQUIRE(player.rawGuid == "0x10000000032");
    REQUIRE(player.gender == 1);
    REQUIRE(player.displayId == 51);
    REQUIRE(player.targetGuid);
    REQUIRE(*player.targetGuid == 0x20000000b8bull);
    REQUIRE(player.equipment.size() == 1);
    REQUIRE(player.equipment[0].slot == 15);
    REQUIRE(player.equipment[0].displayId == 5195);
    REQUIRE(player.equipment[0].inventoryType == 13);

    auto creatures = recording.sampleCreatures(1250.0, 1);
    REQUIRE(creatures.size() == 1);
    REQUIRE(creatures[0].creature.guid == 0x20000000b8bull);
    REQUIRE(creatures[0].creature.entry == 2955);
    REQUIRE(creatures[0].creature.displayId == 390);
    REQUIRE(creatures[0].creature.x == Catch::Approx(-624.0f));
    REQUIRE(creatures[0].creature.y == Catch::Approx(-4249.0f));
    REQUIRE(creatures[0].creature.hp == 48);
    REQUIRE(creatures[0].creature.combat == true);
    REQUIRE(creatures[0].moving == true);
}

TEST_CASE("GodviewRecording reports malformed JSONL with line number", "[godview][recording]") {
    auto path = writeTempRecording(
        "godview_bad",
        R"JSON({"t":1,"ms":1,"map":0,"players":[{"guid":1,"name":"Ok","level":1,"race":1,"class":1,"x":0,"y":0,"z":0,"o":0,"hp":1,"maxhp":1,"target":0,"combat":false}]}
not-json
)JSON");

    GodviewRecording recording;
    std::string error;
    REQUIRE_FALSE(recording.load(path.string(), error));
    REQUIRE(error.find("line 2") != std::string::npos);
}
