#include "dom.h"
#include "render.h"
#include "tb.h"
#include "view.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static std::string slurp(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

int main(int argc, char **argv) {
  bool regenerate = false;
  std::string prefix;   // 源目录前缀(ctest 传绝对路径;手动跑可为空→相对当前目录)
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "-u") regenerate = true;
    else prefix = argv[i];
  }

  const char *fixtures[] = { "hello", "forms", "nav" };
  int failures = 0;
  for (const char *name : fixtures) {
    std::string dir = prefix + "tests/goldens";
    std::string html = slurp(dir + "/fixtures/" + name + ".html");
    std::string golden_path = dir + "/goldens/" + name + ".golden";

    tb_dom *d = tb_dom_parse(html.data(), html.size());
    if (!d) { std::cerr << name << ": parse failed\n"; failures++; continue; }
    std::string url = "http://example.test/" + std::string(name);
    tb_view *v = tb_render(d, url.c_str(), 200);
    char *dump = tb_view_dump(v);
    tb_dom_free(d);
    tb_view_free(v);

    std::string got(dump);
    tb_free(dump);
    if (regenerate) {
      std::ofstream out(golden_path, std::ios::binary);
      out << got;
      std::cout << name << ": regenerated\n";
      continue;
    }
    std::string want = slurp(golden_path);
    if (got != want) {
      std::cerr << "MISMATCH " << name << "\n--- want ---\n" << want
                << "--- got ---\n" << got << "---\n";
      failures++;
    } else {
      std::cout << name << ": OK\n";
    }
  }
  if (failures) { std::cerr << failures << " golden mismatch(es)\n"; return 1; }
  return 0;
}
