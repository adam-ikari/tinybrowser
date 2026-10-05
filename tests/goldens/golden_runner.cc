#include "tb.h"
#include "view.h"
#include "js_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static std::string slurp(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

/* Drive the real seam: open(NULL) / load_document / render → dump. */
static char *render_html(const char *html, size_t hlen, const char *url) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  if (!eng) return NULL;
  void *h = eng->open(eng, NULL);
  if (!h) return NULL;
  if (eng->load_document(eng, h, html, hlen, url) != 0) {
    eng->close(eng, h);
    return NULL;
  }
  tb_view *v = tb_view_new();
  if (eng->render(eng, h, url, 200, v) != 0) {
    tb_view_free(v);
    eng->close(eng, h);
    return NULL;
  }
  char *dump = tb_view_dump(v);
  tb_view_free(v);
  eng->close(eng, h);
  return dump;
}

int main(int argc, char **argv) {
  bool regenerate = false;
  std::string prefix;   /* ctest 传绝对路径; 手动跑可为空 */
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "-u") regenerate = true;
    else prefix = argv[i];
  }

  const char *fixtures[] = { "hello", "forms", "nav", "entities", "scriptpage",
                             "dynamic" };
  int nfixtures = (int)(sizeof(fixtures) / sizeof(fixtures[0]));
  int failures = 0;

  for (int fi = 0; fi < nfixtures; fi++) {
    const char *name = fixtures[fi];
    std::string dir = prefix + "tests/goldens";
    std::string html_path = dir + "/fixtures/" + name + ".html";
    std::string golden_path = dir + "/goldens/" + name + ".golden";

    std::string html = slurp(html_path);
    std::string url = "http://example.test/" + std::string(name);

    char *dump = render_html(html.data(), html.size(), url.c_str());
    if (!dump) {
      std::cerr << name << ": render failed\n";
      failures++;
      continue;
    }

    std::string got(dump);
    tb_free(dump);

    if (regenerate) {
      std::ofstream out(golden_path, std::ios::binary);
      out << got;
      std::cout << name << ": regenerated\n";
      continue;
    }

    std::string want = slurp(golden_path);
    if (want.empty()) {
      std::cerr << name << ": golden file missing or empty (use -u to generate)\n";
      failures++;
      continue;
    }
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
