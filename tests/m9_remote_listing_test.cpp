#include <cassert>
#include <iostream>
#include <vector>

#include "protocol/RemoteListing.hpp"
#include "protocol/StateMachine.hpp"

using namespace ftpclient::protocol;

int main() {
    std::vector<RemoteListingEntry> entries;
    const std::string listing =
        "type=cdir;modify=20260818120000; .\r\n"
        "type=pdir; ..\r\n"
        "type=dir;modify=20260818120100; folder with spaces\r\n"
        "type=file;size=12;modify=20260818120200; report final.bin\r\n"
        "type=file;size=0;unique=opaque; empty.txt\r\n";

    assert(parse_mlsd_listing(listing, entries));
    assert(entries.size() == 3);
    assert(entries[0].type == "dir");
    assert(entries[0].name == "folder with spaces");
    assert(entries[1].type == "file");
    assert(entries[1].name == "report final.bin");
    assert(entries[1].has_size && entries[1].size_bytes == 12);
    assert(entries[2].has_size && entries[2].size_bytes == 0);

    RemoteListingEntry malformed;
    assert(!parse_mlsd_entry("type=file;size=bad; invalid.bin", malformed));
    assert(!parse_mlsd_entry("type=file;missing-separator", malformed));
    assert(!parse_mlsd_entry("type=file;size=1; ../escape.bin", malformed));
    assert(!parse_mlsd_entry("type=file;size=1; /absolute.bin", malformed));
    assert(!parse_mlsd_entry("type=dir; name/with-separator", malformed));
    assert(!parse_mlsd_entry("type=file;size=1; C:drive.bin", malformed));

    StateMachine machine;
    machine.set_state(ProtocolState::AUTHENTICATED);
    assert(machine.transition(FtpCommand::MLSD, 0) == TransitionResult::INVALID_STATE);
    machine.set_state(ProtocolState::DATA_CONNECTING);
    assert(machine.transition(FtpCommand::MLSD, 0) == TransitionResult::SUCCESS);
    assert(machine.transition(FtpCommand::MLSD, 226) == TransitionResult::SUCCESS);
    assert(machine.is_authenticated());

    std::cout << "M9 remote listing tests passed" << std::endl;
    return 0;
}
