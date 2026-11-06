#include "pl64/control.hpp"
#include <array>
#include <cstring>
#include <flatbuffers/reflection.h>
#include <fstream>
#include <iostream>
#include <map>
#include <mcap/crc32.hpp>
#include <mcap/reader.hpp>
int main(int argc, char **argv) try {
    if (argc != 2)
        throw std::invalid_argument("Usage: pl64-mcap-check FILE.mcap");
    std::ifstream file(argv[1], std::ios::binary);
    file.seekg(-8, std::ios::end);
    std::array<unsigned char, 8> magic{};
    file.read(reinterpret_cast<char *>(magic.data()), magic.size());
    if (!file || magic != std::array<unsigned char, 8>{0x89, 'M', 'C', 'A', 'P', '0', '\r', '\n'})
        throw std::runtime_error("MCAP is incomplete; recover it before checking");
    mcap::McapReader reader;
    auto status = reader.open(argv[1]);
    if (!status.ok())
        throw std::runtime_error(status.message);
    mcap::TypedRecordReader integrity(*reader.dataSource(), 8, reader.dataSource()->size() - 8);
    integrity.onChunk = [](const mcap::Chunk &chunk, mcap::ByteOffset) {
        if (!chunk.compression.empty())
            throw std::runtime_error("This checker accepts uncompressed MCAP only");
        if (chunk.uncompressedCrc && mcap::internal::crc32Final(mcap::internal::crc32Update(
                                         mcap::internal::CRC32_INIT, chunk.records,
                                         chunk.uncompressedSize)) != chunk.uncompressedCrc)
            throw std::runtime_error("MCAP chunk CRC mismatch");
    };
    pl64::Json boundaries = pl64::Json::array();
    integrity.onMetadata = [&](const mcap::Metadata &metadata, mcap::ByteOffset) {
        boundaries.push_back({{"name", metadata.name}, {"metadata", metadata.metadata}});
    };
    while (integrity.next()) {
    }
    if (!integrity.status().ok())
        throw std::runtime_error(integrity.status().message);
    std::map<std::string, std::uint64_t> topics;
    std::map<std::string, std::map<std::string, std::uint64_t>> kinds;
    std::uint64_t bytes = 0;
    std::vector<std::uint64_t> schema_data, payload;
    for (const auto &view :
         reader.readMessages([](const auto &error) { throw std::runtime_error(error.message); })) {
        if (!view.schema || view.schema->encoding != "flatbuffer" ||
            view.channel->messageEncoding != "flatbuffer")
            throw std::runtime_error("Unexpected schema encoding");
        schema_data.resize((view.schema->data.size() + 7) / 8);
        std::memcpy(schema_data.data(), view.schema->data.data(), view.schema->data.size());
        flatbuffers::Verifier schema_verify(
            reinterpret_cast<const std::uint8_t *>(schema_data.data()), view.schema->data.size());
        if (!reflection::VerifySchemaBuffer(schema_verify))
            throw std::runtime_error("Invalid binary schema");
        const auto *schema = reflection::GetSchema(schema_data.data());
        if (!schema->root_table())
            throw std::runtime_error("Schema has no root table");
        payload.resize((view.message.dataSize + 7) / 8);
        std::memcpy(payload.data(), view.message.data, view.message.dataSize);
        if (!flatbuffers::Verify(*schema, *schema->root_table(),
                                 reinterpret_cast<const std::uint8_t *>(payload.data()),
                                 view.message.dataSize))
            throw std::runtime_error("Invalid FlatBuffer record");
        ++topics[view.channel->topic];
        if (const auto *field = schema->root_table()->fields()->LookupByKey("kind")) {
            const auto code = flatbuffers::GetAnyFieldI(
                *flatbuffers::GetAnyRoot(reinterpret_cast<const std::uint8_t *>(payload.data())),
                *field);
            const auto index = field->type()->index();
            std::string name = std::to_string(code);
            if (index >= 0 && schema->enums() && unsigned(index) < schema->enums()->size()) {
                if (const auto *value = schema->enums()->Get(index)->values()->LookupByKey(code))
                    name = value->name()->str();
            }
            ++kinds[view.channel->topic][name];
        }
        bytes += view.message.dataSize;
    }
    std::cout << pl64::Json{{"valid", true},
                            {"topics", topics},
                            {"record_kinds", kinds},
                            {"payload_bytes", bytes},
                            {"boundaries", boundaries}}
                     .dump(2)
              << '\n';
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
