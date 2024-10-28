#include <filesystem>
#include <chrono>

#include <stp_parser.hpp>
#include <LiteMath.h>


using namespace STEP;

void print_nurbs(const RawNURBS &nurbs) {
    auto points = nurbs.points;
    for (size_t i = 0; i < points.rows_count(); i++) {
        for (size_t j = 0; j < points.cols_count(); j++) {
            auto index = std::make_pair(i, j);
            auto point = points[index];
            std::cout << "(" << point.x << " " << point.y << " " << point.z << "), ";
        }
        std::cout << std::endl;
    }

    auto u_knots = nurbs.u_knots;
    for (size_t i = 0; i < u_knots.size(); i++) {
        std::cout << u_knots[i] << " ";
    }
    std::cout << std::endl;

    auto v_knots = nurbs.v_knots;
    for (size_t i = 0; i < v_knots.size(); i++) {
        std::cout << v_knots[i] << " ";
    }
    std::cout << std::endl;

    auto weights = nurbs.weights;
    for (size_t i = 0; i < weights.rows_count(); i++) {
        for (size_t j = 0; j < weights.cols_count(); j++) {
            auto index = std::make_pair(i, j);
            auto w = weights[index];
            std::cout << w << " ";
        }
        std::cout << std::endl;
    }
}

int main(int argc, const char **argv) {
  if (argc != 2) {
    std::cout << "Usage: parse_and_print <path_to_stp_file>" << std::endl;
    return 0;
  }
  std::filesystem::path stp_path = argv[1];
  std::cout << "Parsing started..." << std::endl;
  auto tick_start = std::chrono::high_resolution_clock::now();
  auto entities = STEP::parse(stp_path);
  auto nurbsV = STEP::allNURBS(entities);
  auto tick_end = std::chrono::high_resolution_clock::now();
  std::cout << "Parsing finished successfully." << std::endl;
  float time = 
      std::chrono::duration_cast<std::chrono::milliseconds>(tick_end-tick_start).count()/1000.0f;
  std::cout << "Parsing time: " << time << "s." << std::endl;

  int counter = 0;
  for (auto &nurbs: nurbsV) {
    std::cout << "### NURBS " << counter++ << " ###" << std::endl;
    print_nurbs(nurbs);
  }
  return 0;
}
