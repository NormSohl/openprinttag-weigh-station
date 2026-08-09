#include "match_extract.h"
#include <cstdio>
static int fails = 0;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL %s\n", msg); fails++; } }while(0)

static uint32_t add(const char* ven, const char* mat, const char* abbr,
                    float nom, const char* pkg="", const char* mu="",
                    unsigned long long gtin=0) {
    ProductRecord p; p.id = (uint32_t)sProducts.size()+1;
    strncpy(p.vendor,ven,63); strncpy(p.material,mat,63); strncpy(p.abbr,abbr,15);
    p.nom_g = nom; strncpy(p.pkg_uuid,pkg,32); strncpy(p.mat_uuid,mu,32);
    p.gtin = gtin; p.valid = true; sProducts.push_back(p); return p.id;
}
static ProductRecord probe(const char* ven, const char* mat, float nom,
                           const char* pkg="", const char* mu="",
                           unsigned long long gtin=0) {
    ProductRecord q;
    strncpy(q.vendor,ven,63); strncpy(q.material,mat,63); q.nom_g=nom;
    strncpy(q.pkg_uuid,pkg,32); strncpy(q.mat_uuid,mu,32); q.gtin=gtin;
    return q;
}
int main() {
    // normEq_ directly
    CHECK(normEq_("eSun","ESUN"),            "case-insensitive");
    CHECK(normEq_("PLA Summer","PLA  Summer"),"collapses whitespace");
    CHECK(!normEq_("PLA","PLA+"),            "PLA+ must NOT merge into PLA");
    CHECK(!normEq_("PLA","PETG"),            "different materials differ");
    CHECK(normEq_("",""),                    "both empty");
    CHECK(!normEq_("","PLA"),                "empty vs set");
    CHECK(!normEq_("PLA","PL"),              "prefix is not equal");
    CHECK(!normEq_("PL","PLA"),              "prefix is not equal (reversed)");

    // nomEq_ — 0 means "cannot tell", so it must not block a match
    CHECK(nomEq_(1000,1000), "same nominal");
    CHECK(!nomEq_(1000,5000),"1 kg is not 5 kg");
    CHECK(nomEq_(0,1000),    "unknown nominal cannot distinguish");
    CHECK(nomEq_(1000,0),    "unknown nominal cannot distinguish (reversed)");

    const char* PKG_A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char* MAT_X = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    uint32_t p1 = add("Prusament","PETG Orange","PETG",1000, PKG_A, MAT_X, 8595581746018ULL);
    uint32_t p2 = add("Prusament","PETG Orange","PETG",5000, "",    MAT_X, 8595581746025ULL);
    uint32_t p3 = add("eSun",     "PLA+ Black", "PLA+", 1000);

    int i;
    i = findProduct_(probe("","",0,PKG_A));
    CHECK(i>=0 && sProducts[i].id==p1, "1) package_uuid wins");
    i = findProduct_(probe("","",0,"",  "", 8595581746025ULL));
    CHECK(i>=0 && sProducts[i].id==p2, "2) gtin picks the 5 kg");
    i = findProduct_(probe("","",5000,"",MAT_X));
    CHECK(i>=0 && sProducts[i].id==p2, "3) material_uuid + nominal separates sizes");
    i = findProduct_(probe("","",1000,"",MAT_X));
    CHECK(i>=0 && sProducts[i].id==p1, "3) material_uuid + nominal picks the 1 kg");
    i = findProduct_(probe("PRUSAMENT","petg orange",1000));
    CHECK(i>=0 && sProducts[i].id==p1, "4) names, case-insensitive");
    i = findProduct_(probe("Prusament","PETG Orange",5000));
    CHECK(i>=0 && sProducts[i].id==p2, "4) names + nominal separates sizes");
    i = findProduct_(probe("eSun","PLA Black",1000));
    CHECK(i<0,                          "PLA Black must not match PLA+ Black");
    i = findProduct_(probe("Hatchbox","PLA Red",1000));
    CHECK(i<0,                          "unknown product creates rather than matches");
    (void)p3;

    // A tag with a package_uuid we have never seen must NOT fall through to a
    // name match on a different SKU that happens to share the name.
    i = findProduct_(probe("Prusament","PETG Orange",1000,"ffffffffffffffffffffffffffffffff"));
    CHECK(i>=0 && sProducts[i].id==p1,
          "unknown package_uuid still falls through to the name rule");

    // Empty probe must match nothing, not the first row.
    ProductRecord empty;
    CHECK(findProduct_(empty) < 0, "an empty probe matches nothing");

    printf(fails ? "\nFAIL: %d\n" : "\nPASS: all matcher checks (%d failures)\n", fails);
    return fails != 0;
}
