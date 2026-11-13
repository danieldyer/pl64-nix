// Replay retained serial reads through the actual production acquisition path.
#include "ins/acquisition.hpp"
#include <fstream>
#include <iostream>
#include <memory>

template <class T> T read(std::istream &in) {
    T value{};
    in.read(reinterpret_cast<char *>(&value), sizeof(value));
    if (!in)
        throw std::runtime_error("Truncated wire input");
    return value;
}
template <class T> void write(std::ostream &out, T value) {
    out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::ofstream output(argv[2], std::ios::binary);
    if (!input || !output)
        return 2;
    ins::Config config; // Recorded sensor_to_body is identity on these sessions.
    std::unique_ptr<ins::Acquisition> acquisition;
    std::uint64_t count = 0, checksum_errors = 0;
    auto reset = [&] {
        if (acquisition)
            checksum_errors += acquisition->imu_stats().bad_checksums;
        acquisition = std::make_unique<ins::Acquisition>(config);
        acquisition->on_imu = [&](const ins::ImuSample &sample) {
            write<std::uint64_t>(output, sample.counter);
            write<std::int64_t>(output, sample.stamp.measurement.count());
            write<std::int64_t>(output, sample.stamp.arrival.count());
            for (double v : sample.omega)
                write(output, v);
            for (double v : sample.force)
                write(output, v);
            write<std::uint32_t>(output, (sample.gap ? 1 : 0) | (sample.saturated ? 2 : 0));
            ++count;
        };
    };
    reset();
    while (input.peek() != EOF) {
        const auto source = read<std::uint8_t>(input);
        ins::Record record;
        record.kind = ins::Kind::raw;
        record.source = static_cast<ins::Source>(source);
        record.stamp.arrival = ins::Time(read<std::int64_t>(input));
        record.stamp.utc = ins::Time(read<std::int64_t>(input));
        record.stamp.measurement = ins::Time(read<std::int64_t>(input));
        record.stamp.uncertainty_s = read<double>(input);
        record.stamp.sequence = read<std::uint64_t>(input);
        const auto size = read<std::uint32_t>(input);
        record.payload.resize(size);
        input.read(reinterpret_cast<char *>(record.payload.data()), size);
        if (!input)
            throw std::runtime_error("Truncated serial payload");
        if (source == 254)
            reset();
        else
            acquisition->accept(record);
    }
    checksum_errors += acquisition->imu_stats().bad_checksums;
    std::cout << "{\"samples\":" << count << ",\"bad_imu_checksums\":" << checksum_errors << "}\n";
}
