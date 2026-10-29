#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>

// Bloqueador por dominio. Es inmutable después de cargar las listas, así que
// puede consultarse desde cualquier hilo sin bloqueos (solo el contador es atómico).
//
// Formatos soportados en ui/filters/*.txt:
//   - hosts:        0.0.0.0 ads.ejemplo.com
//   - Adblock:      ||ads.ejemplo.com^
//   - dominio solo: ads.ejemplo.com
// No soporta reglas con rutas, comodines, opciones ($...) ni filtros cosméticos (##).
class AdBlock {
 public:
  void LoadDirectory(const std::filesystem::path& dir);
  void LoadFile(const std::filesystem::path& file);

  // true si el host de la URL (o alguno de sus dominios padre) está en la lista.
  bool ShouldBlock(const std::string& url) const;

  uint64_t blocked() const { return blocked_.load(); }
  size_t rules() const { return domains_.size(); }

 private:
  void AddRule(std::string line);

  std::unordered_set<std::string> domains_;
  mutable std::atomic<uint64_t> blocked_{0};
};
