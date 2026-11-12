#ifndef PVZ2NATIVE_CREDITS_DATA_H
#define PVZ2NATIVE_CREDITS_DATA_H

#include <cstddef>

namespace pvz2native {

enum class CreditLang {
    ES = 0,
    EN = 1,
    FR = 2,
    DE = 3,
    IT = 4,
    PT = 5,
    Count = 6
};

enum class CreditKind {
    Space = 0,
    TopHeader = 1,
    Header = 2,
    Title = 3,
    Name = 4,
    NoteName = 5,
    NoteText = 6
};

struct CreditEntry {
    CreditKind kind;
    const char *text[6];
};

extern const CreditEntry kCreditEntries[];
extern const size_t kCreditEntriesCount;

const char *credit_get_text(const CreditEntry &entry, CreditLang lang);

} // namespace pvz2native

#endif // PVZ2NATIVE_CREDITS_DATA_H
