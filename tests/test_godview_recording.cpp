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
