#include <lci-irregular/irregular_runtime.hpp>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace lci_irregular {
namespace {

std::string json_escape(const std::string& value) {
  std::ostringstream escaped;
  for (unsigned char character : value) {
    switch (character) {
    case '"':
      escaped << "\\\"";
      break;
    case '\\':
      escaped << "\\\\";
      break;
    case '\b':
      escaped << "\\b";
      break;
    case '\f':
      escaped << "\\f";
      break;
    case '\n':
      escaped << "\\n";
      break;
    case '\r':
      escaped << "\\r";
      break;
    case '\t':
      escaped << "\\t";
      break;
    default:
      if (character < 0x20) {
        escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character)
                << std::dec;
      } else {
        escaped << static_cast<char>(character);
      }
    }
  }
  return escaped.str();
}

struct NamedOperation {
  const char* name;
  const AmOperationProfile AmWorkerProfile::* profile;
};

constexpr NamedOperation operations[] = {
    {"flush", &AmWorkerProfile::flush},
    {"progress", &AmWorkerProfile::progress},
    {"remote_receive", &AmWorkerProfile::remote_receive},
    {"loopback_receive", &AmWorkerProfile::loopback_receive},
};

void write_worker_rows(std::ostream& output, int rank, const AmExchangeProfile& profile, const std::string& scope,
                       const AmWorkerProfile& worker) {
  for (const auto& operation : operations) {
    const AmOperationProfile& values = worker.*(operation.profile);
    output << "{\"rank\":" << rank << ",\"exchange\":" << profile.exchange_sequence << ",\"name\":\""
           << json_escape(profile.name) << "\",\"scope\":\"" << scope << "\",\"operation\":\"" << operation.name
           << "\",\"calls\":" << values.calls << ",\"records\":" << values.records
           << ",\"payload_bytes\":" << values.payload_bytes << ",\"elapsed_nanoseconds\":" << values.elapsed_nanoseconds
           << "}\n";
  }
}

} // namespace

void IrregularRuntime::write_profiles() {
  if (!profiling_options_.enabled) {
    throw std::logic_error("LCI irregular profiling is disabled");
  }
  if (profiling_options_.output_directory.empty()) {
    throw std::logic_error("LCI irregular profiling output directory is not configured");
  }

  std::lock_guard<std::mutex> writer_lock(profile_write_mutex_);
  std::vector<AmExchangeProfile> profiles;
  {
    std::lock_guard<std::mutex> queue_lock(profile_mutex_);
    profiles.swap(pending_profiles_);
  }
  if (profiles.empty()) {
    return;
  }

  const std::filesystem::path directory(profiling_options_.output_directory);
  const std::filesystem::path output_path =
      directory / (profiling_options_.file_prefix + ".rank-" + std::to_string(rank_) + ".jsonl");

  try {
    std::filesystem::create_directories(directory);
    const auto mode = std::ios::out | (profile_output_started_ ? std::ios::app : std::ios::trunc);
    std::ofstream output(output_path, mode);
    if (!output) {
      throw std::runtime_error("unable to open output file");
    }

    for (const auto& profile : profiles) {
      write_worker_rows(output, rank_, profile, "aggregate", profile.aggregate);
      for (size_t worker_index = 0; worker_index < profile.workers.size(); ++worker_index) {
        write_worker_rows(output, rank_, profile, "worker-" + std::to_string(worker_index),
                          profile.workers[worker_index]);
      }
    }
    output.close();
    if (!output) {
      throw std::runtime_error("unable to complete output file");
    }
    profile_output_started_ = true;
  } catch (const std::exception& error) {
    std::lock_guard<std::mutex> queue_lock(profile_mutex_);
    pending_profiles_.insert(pending_profiles_.begin(), std::make_move_iterator(profiles.begin()),
                             std::make_move_iterator(profiles.end()));
    throw std::runtime_error("unable to write LCI irregular profile " + output_path.string() + ": " + error.what());
  }
}

} // namespace lci_irregular
