#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Coloration du code dans le volet d'aperçu : un découpage simple (mots-clés, chaînes,
// commentaires, nombres, directives, balises) par langage, rendu en RTF pour un RichEdit.

enum SynKind : uint8_t { SYN_TEXT, SYN_KEYWORD, SYN_STRING, SYN_COMMENT, SYN_NUMBER, SYN_PREPROC, SYN_TAG };

struct SynSpan {
    uint32_t start, len;
    SynKind kind;
};

bool Syntax_Known(const wchar_t* ext);  // « .cpp », « .py »… (sans casse)
// Morceaux colorés (le reste est du texte), dans l'ordre, sans chevauchement.
std::vector<SynSpan> Syntax_Spans(const std::wstring& text, const wchar_t* ext);
// Document RTF (police Consolas ; taille fixée ensuite par le contrôle).
std::string Syntax_Rtf(const std::wstring& text, const std::vector<SynSpan>& spans);
