// Native check of src/web_page.h. Run from the repo root:
//   g++ -std=c++17 -include memory -Itools/store/shim -Isrc tools/page/test_page.cpp -o /tmp/tp && /tmp/tp
#include "web_page.h"
#include <cassert>
#include <string>
int main(){
  Page p("<html>"); std::string want="<html>";
  for(int i=0;i<5000;i++){ String s(i); p+=s; p+=','; want+=std::to_string(i)+","; }
  p+="</html>"; want+="</html>";
  assert(p.length()==want.size() && !p.failed());
  auto q=std::make_shared<Page>(); q->swap(p); assert(p.length()==0);
  std::string got; uint8_t b[1000]; size_t n; while((n=q->read(b,sizeof b))) got.append((char*)b,n);
  assert(got==want);
  Page f; f+=(const char*)nullptr; assert(f.failed());
  puts("page ok");
}
