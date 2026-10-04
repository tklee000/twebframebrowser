#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace TWebFrame::Internal::CssSyntax {

inline bool Space(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f'; }
inline bool Name(wchar_t c) { return c >= 128 || std::iswalnum(c) || c == L'-' || c == L'_'; }
inline std::wstring Trim(std::wstring_view text) {
    while (!text.empty() && Space(text.front())) text.remove_prefix(1);
    while (!text.empty() && Space(text.back())) text.remove_suffix(1);
    return std::wstring(text);
}
inline int Hex(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}
// Return the first code unit after a CSS escape, including its optional space.
inline size_t EscapeEnd(std::wstring_view text, size_t position) {
    if (position >= text.size() || text[position] != L'\\') return position;
    ++position;
    if (position == text.size()) return position;
    if (Hex(text[position]) < 0) return position + 1;
    for (int count = 0; position < text.size() && count < 6 && Hex(text[position]) >= 0; ++count) ++position;
    if (position < text.size() && Space(text[position])) {
        const auto c = text[position++];
        if (c == L'\r' && position < text.size() && text[position] == L'\n') ++position;
    }
    return position;
}
inline std::wstring Decode(std::wstring_view text) {
    std::wstring result;
    for (size_t i = 0; i < text.size();) {
        if (text[i] != L'\\') { result += text[i++]; continue; }
        const auto end = EscapeEnd(text, i);
        ++i;
        if (i >= text.size()) { result += wchar_t(0xfffd); break; }
        if (text[i] == L'\n' || text[i] == L'\r' || text[i] == L'\f') { i = end; continue; }
        unsigned code = 0;
        if (Hex(text[i]) >= 0) {
            for (int count = 0; i < text.size() && count < 6 && Hex(text[i]) >= 0; ++count)
                code = code * 16 + Hex(text[i++]);
        } else code = text[i];
        if (!code || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) code = 0xfffd;
        if (code > 0xffff) { code -= 0x10000; result += wchar_t(0xd800 + (code >> 10)); result += wchar_t(0xdc00 + (code & 1023)); }
        else result += wchar_t(code);
        i = end;
    }
    return result;
}
inline size_t IdentifierEnd(std::wstring_view text, size_t position) {
    while (position < text.size()) {
        if (Name(text[position])) ++position;
        else if (text[position] == L'\\' && position + 1 < text.size() && !Space(text[position + 1])) position = EscapeEnd(text, position);
        else break;
    }
    return position;
}
inline std::wstring Comments(std::wstring_view text) {
    std::wstring result;
    wchar_t quote = 0;
    for (size_t i = 0; i < text.size();) {
        const auto c = text[i];
        if (c == L'\\') { const auto end = EscapeEnd(text, i); result.append(text.substr(i, end - i)); i = end; continue; }
        if (quote) { result += c; if (c == quote || c == L'\n' || c == L'\r' || c == L'\f') quote = 0; ++i; continue; }
        if (c == L'\'' || c == L'"') { quote = c; result += c; ++i; continue; }
        if (c == L'/' && i + 1 < text.size() && text[i + 1] == L'*') {
            const auto end = text.find(L"*/", i + 2);
            result += L' '; // Keep adjacent identifiers/numbers from merging.
            i = end == std::wstring_view::npos ? text.size() : end + 2;
        } else { result += c == 0 ? wchar_t(0xfffd) : c; ++i; }
    }
    return result;
}
// Scan component values. Delimiters in strings, escapes and blocks are opaque.
inline size_t Find(std::wstring_view text, std::wstring_view delimiters, size_t start = 0) {
    std::vector<wchar_t> blocks;
    wchar_t quote = 0;
    for (size_t i = start; i < text.size(); ++i) {
        const auto c = text[i];
        if (c == L'\\') { i = EscapeEnd(text, i) - 1; continue; }
        if (quote) { if (c == quote || c == L'\n' || c == L'\r' || c == L'\f') quote = 0; continue; }
        if (c == L'\'' || c == L'"') { quote = c; continue; }
        if (blocks.empty() && delimiters.find(c) != std::wstring_view::npos) return i;
        if (c == L'(' || c == L'[' || c == L'{') blocks.push_back(c == L'(' ? L')' : c == L'[' ? L']' : L'}');
        else if (!blocks.empty() && c == blocks.back()) blocks.pop_back();
    }
    return std::wstring_view::npos;
}
inline size_t Close(std::wstring_view text, size_t open) {
    if (open >= text.size()) return text.size();
    const auto close = text[open] == L'(' ? L')' : text[open] == L'[' ? L']' : L'}';
    const auto end = Find(text, std::wstring(1, close), open + 1);
    return end == std::wstring_view::npos ? text.size() : end;
}
inline std::vector<std::wstring> Split(std::wstring_view text, wchar_t delimiter, bool keepEmpty = false) {
    std::vector<std::wstring> result;
    size_t start = 0;
    do {
        auto end = Find(text, std::wstring(1, delimiter), start);
        if (end == std::wstring_view::npos) end = text.size();
        auto item = Trim(text.substr(start, end - start));
        if (keepEmpty || !item.empty()) result.push_back(std::move(item));
        start = end + 1;
    } while (start <= text.size());
    return result;
}
inline std::vector<std::wstring> Words(std::wstring_view text) {
    std::vector<std::wstring> result;
    for (size_t start = 0; start < text.size();) {
        while (start < text.size() && Space(text[start])) ++start;
        auto end = Find(text, L" \t\r\n\f", start);
        if (end == std::wstring_view::npos) end = text.size();
        if (end > start) result.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return result;
}
struct Declaration { std::wstring name, value; bool important = false; };
inline bool ParseDeclaration(std::wstring_view text, Declaration& result) {
    const auto colon = Find(text, L":");
    if (colon == std::wstring_view::npos) return false;
    auto name = Trim(text.substr(0, colon));
    if (name.empty() || IdentifierEnd(name, 0) != name.size() || (name[0] != L'\\' && name[0] != L'-' && name[0] != L'_' && name[0] < 128 && !std::iswalpha(name[0]))) return false;
    result.name = Decode(name);
    if (result.name.rfind(L"--", 0) != 0)
        std::transform(result.name.begin(), result.name.end(), result.name.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    result.value = Trim(text.substr(colon + 1)); result.important = false;
    const auto bang = Find(result.value, L"!");
    if (bang != std::wstring::npos) {
        auto keyword = Decode(Trim(std::wstring_view(result.value).substr(bang + 1)));
        std::transform(keyword.begin(), keyword.end(), keyword.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
        if (keyword == L"important") { result.important = true; result.value = Trim(std::wstring_view(result.value).substr(0, bang)); }
    }
    return !result.value.empty() || result.name.rfind(L"--", 0) == 0;
}
inline std::vector<Declaration> Declarations(std::wstring_view source) {
    std::vector<Declaration> result;
    const auto text = Comments(source);
    for (const auto& part : Split(text, L';')) {
        Declaration item;
        if (ParseDeclaration(part, item)) result.push_back(std::move(item));
    }
    return result;
}

} // namespace TWebFrame::Internal::CssSyntax
