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
      /* 写失败必须说出来。原先无条件打印 "regenerated",于是 ofstream 打不开
       * (路径不对/只读目录)时它照样报成功 —— 实测用相对 prefix 运行时
       * 6 个文件全部「regenerated」而磁盘上 mtime 一动没动,白跑一轮。
       * 这正是「绿色但没在测」那一类:工具自己骗自己。 */
      std::ofstream out(golden_path, std::ios::binary);
      if (!out) {
        std::cerr << name << ": 写不进去 " << golden_path << "\n";
        failures++;
        continue;
      }
      out << got;
      out.close();
      if (!out) {
        std::cerr << name << ": 写入中断 " << golden_path << "\n";
        failures++;
        continue;
      }
      std::cout << name << ": regenerated -> " << golden_path << "\n";
      continue;
    }

    std::string want = slurp(golden_path);
    if (want.empty()) {
      /* 「missing」与「empty」要分开说,并把路径打出来。合在一句里时,路径写错
       * 会被误读成「文件该生成」,于是有人去跑 -u —— 而 -u 写的也是同一个错
       * 路径,于是双方一起绿。实测踩过:相对 prefix 下 6 个 golden 全被判
       * 「missing or empty」,真正原因是路径解析错了。 */
      std::cerr << name << ": golden " << (std::ifstream(golden_path) ? "是空的: " : "不存在: ")
                << golden_path << "\n";
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
