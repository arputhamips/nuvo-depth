#include "depthd/semantic_types.hpp"

namespace depthd {

const char* semantic_class_name(const SemanticClass value) noexcept {
  switch (value) {
    case SemanticClass::kUnknown:
      return "unknown";
    case SemanticClass::kHuman:
      return "human";
    case SemanticClass::kAnimal:
      return "animal";
    case SemanticClass::kVegetation:
      return "vegetation";
    case SemanticClass::kWall:
      return "wall";
    case SemanticClass::kFence:
      return "fence";
    case SemanticClass::kTerrain:
      return "terrain";
    case SemanticClass::kGenericObstacle:
      return "generic_obstacle";
  }
  return "unknown";
}

}  // namespace depthd
