#ifndef TERRAIN_H_
#define TERRAIN_H_

#include <functional>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "glm/glm.hpp"

#include "tetrahedron.h"

#define CHUNK_SIZE 16

/*
Chunks contain the blocks between (0,0,0,0) and (15, 15, 15, 15) plus the offset given at creation time.

*/

struct hashVec {
  size_t operator()(const glm::ivec4& c) const {
    size_t hash_x = std::hash<int>{}(c[0]);
    size_t hash_y = std::hash<int>{}(c[1]);
    size_t hash_z = std::hash<int>{}(c[2]);
    size_t hash_w = std::hash<int>{}(c[3]);
    hash_x ^= hash_y + 0x9e3779b97f4a7c16 + (hash_x << 6) + (hash_x >> 2);
    hash_x ^= hash_z + 0x9e3779b97f4a7c16 + (hash_x << 6) + (hash_x >> 2);
    hash_x ^= hash_w + 0x9e3779b97f4a7c16 + (hash_x << 6) + (hash_x >> 2);
    return hash_x;
  }
};

class Terrain {
 public:
  class Block {
   public:
    Block(glm::ivec4 c);

    std::vector<Tetrahedron> GetTets();
   private:
    std::vector<Tetrahedron> TesselateCube(std::vector<glm::vec4>& corners,
                                           std::vector<int>& cube_indices);

    glm::ivec4 pos_;
  };

  class Chunk {
   public:
    Chunk(glm::ivec4 c);

    Block* GetBlock(glm::ivec4 c);
   private:
    std::unordered_map<glm::ivec4, Block, hashVec> blocks_;
    glm::ivec4 ref_;
  };

  void GenChunk(glm::ivec4 c);

  Block* GetBlock(glm::ivec4 c);

 private:
  std::unordered_map<glm::ivec4, Chunk, hashVec> chunks_;
};

#endif  // TERRAIN_H_