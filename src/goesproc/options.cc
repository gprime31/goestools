#include "options.h"

#include <stdlib.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iostream>
#include <sstream>

#include "lib/lrit.h"
#include "dir.h"

namespace {

std::vector<std::string> split(std::string in, char delim) {
  std::vector<std::string> items;
  std::istringstream ss(in);
  std::string item;
  while (std::getline(ss, item, delim)) {
    items.push_back(item);
  }
  return items;
}

} // namespace

Options parseOptions(int argc, char** argv) {
  Options opts;

  // Defaults
  opts.shrink = false;

  while (1) {
    static struct option longOpts[] = {
      {"channel", required_argument, 0, 'c'},
      {"shrink", no_argument, 0, 0x1001},
      {"scale", required_argument, 0, 0x1002},
    };
    int i;
    int c = getopt_long(argc, argv, "c:", longOpts, &i);
    if (c == -1) {
      break;
    }

    switch (c) {
    case 0:
      break;
    case 'c':
      opts.channel = optarg;
      break;
    case 0x1001: // --shrink
      opts.shrink = true;
      break;
    case 0x1002: // --scale
      {
        auto parts = split(optarg, 'x');
        if (parts.size() != 2) {
          std::cerr << "Invalid argument to --scale" << std::endl;
          exit(1);
        }
        opts.scale.width = std::stoi(parts[0]);
        opts.scale.height = std::stoi(parts[1]);
        opts.scale.cropWidth = CropWidth::CENTER;
        opts.scale.cropHeight = CropHeight::CENTER;
      }
      break;
    default:
      std::cerr << "Invalid option" << std::endl;
      exit(1);
    }
  }

  // Gather file names from arguments (globs *.lrit in directories).
  std::vector<std::string> paths;
  for (int i = optind; i < argc; i++) {
    struct stat st;
    auto rv = stat(argv[i], &st);
    if (rv < 0) {
      perror("stat");
      exit(1);
    }
    if (S_ISDIR(st.st_mode)) {
      Dir dir(argv[i]);
      auto result = dir.matchFiles("*.lrit");
      paths.insert(paths.end(), result.begin(), result.end());
    } else {
      paths.push_back(argv[i]);
    }
  }

  if (paths.empty()) {
    std::cerr << "No files to process..." << std::endl;
    exit(0);
  }

  // Load header of all specified files
  for (const auto& p : paths) {
    opts.files.push_back(File(p));
  }

  auto firstHeader = opts.files.front().getHeader<LRIT::PrimaryHeader>();
  opts.fileType = firstHeader.fileType;

  // Verify all files have the same fundamental type
  for (const auto& f : opts.files) {
    auto ph = f.getHeader<LRIT::PrimaryHeader>();
    if (ph.fileType != opts.fileType) {
      std::cerr << "Cannot handle mixed LRIT file types..." << std::endl;
      exit(1);
    }
  }

  return opts;
}
