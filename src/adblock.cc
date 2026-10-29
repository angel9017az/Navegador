#include "adblock.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

bool LooksLikeDomain(const std::string& d) {
  if (d.size() < 3 || d.find('.') == std::string::npos) return false;
  for (char c : d) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'))
      return false;
  }
  return true;
}

std::string HostOf(const std::string& url) {
  size_t s = url.find("://");
  if (s == std::string::npos) return "";
  s += 3;
  size_t e = url.find_first_of("/:?#", s);
  std::string host = url.substr(s, e == std::string::npos ? std::string::npos : e - s);
  std::transform(host.begin(), host.end(), host.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return host;
}

}  // namespace

void AdBlock::AddRule(std::string line) {
  line = Trim(line);
  if (line.empty() || line[0] == '#' || line[0] == '!' || line[0] == '[') return;
  if (line.rfind("@@", 0) == 0) return;  // excepciones: no soportadas
  // Reglas cosméticas (example.com##.ad): NO deben bloquear el dominio entero.
  if (line.find("##") != std::string::npos || line.find("#@#") != std::string::npos ||
      line.find("#?#") != std::string::npos || line.find("#$#") != std::string::npos)
    return;

  std::string d;
  if (line.rfind("||", 0) == 0) {
    size_t e = line.find_first_of("^/$*:|", 2);
    if (e != std::string::npos) {
      if (line[e] != '^') return;            // ruta o comodín: no soportado
      if (line.size() > e + 1) return;       // opciones tras '^': no soportado
    }
    d = line.substr(2, e == std::string::npos ? std::string::npos : e - 2);
  } else {
    size_t hash = line.find('#');            // comentario al final (hosts)
    if (hash != std::string::npos) line = Trim(line.substr(0, hash));
    std::istringstream ss(line);
    std::vector<std::string> tok;
    for (std::string t; ss >> t;) tok.push_back(t);
    if (tok.size() >= 2 && (tok[0] == "0.0.0.0" || tok[0] == "127.0.0.1")) d = tok[1];
    else if (tok.size() == 1) d = tok[0];
  }

  std::transform(d.begin(), d.end(), d.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (d == "localhost" || !LooksLikeDomain(d)) return;
  domains_.insert(d);
}

void AdBlock::LoadFile(const std::filesystem::path& file) {
  std::ifstream in(file);
  for (std::string line; std::getline(in, line);) AddRule(line);
}

void AdBlock::LoadDirectory(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.is_regular_file() && e.path().extension() == ".txt") LoadFile(e.path());
  }
}

bool AdBlock::ShouldBlock(const std::string& url) const {
  const std::string host = HostOf(url);
  if (host.empty()) return false;

  // Prueba host completo y luego cada dominio padre (a.b.com -> b.com), sin llegar al TLD solo.
  size_t pos = 0;
  for (;;) {
    if (domains_.count(host.substr(pos))) {
      ++blocked_;
      return true;
    }
    size_t dot = host.find('.', pos);
    if (dot == std::string::npos) break;
    pos = dot + 1;
    if (host.find('.', pos) == std::string::npos) break;
  }
  return false;
}
