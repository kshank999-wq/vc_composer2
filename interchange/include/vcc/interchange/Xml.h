// A small XML DOM reader and an indenting writer, enough for MusicXML.
//
// Framework-free on purpose: MusicXML import and export live below the
// desktop shell, so they cannot lean on JUCE's XmlDocument, and nothing in
// the project may pull in a third-party parser.
//
// The reader builds a tree of elements. It handles what real MusicXML from
// Finale, Sibelius, Dorico and MuseScore contains: an XML declaration, a
// DOCTYPE naming the external DTD (skipped, never fetched - including an
// internal subset in brackets), comments, processing instructions, CDATA,
// the five predefined entities and numeric character references, and a
// UTF-8 byte order mark. Text is kept per element as the concatenation of
// its direct character data, which is all MusicXML's data model needs
// (it has no mixed content that matters).
//
// Parsing is iterative, so a deeply nested or very large file cannot
// exhaust the call stack, and never throws on malformed input: it reports
// the first problem with its line number.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vcc::interchange::xml {

struct Element {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    /// The element's own character data (entities decoded, CDATA included),
    /// concatenated across any child elements in between.
    std::string text;
    std::vector<Element> children;
    int line = 0;  ///< 1-based line of the start tag.

    /// First child with this name, or nullptr.
    const Element* child(std::string_view child_name) const noexcept;
    Element* child(std::string_view child_name) noexcept;
    /// Every child with this name, in document order.
    std::vector<const Element*> children_named(std::string_view child_name) const;

    bool has_attribute(std::string_view attribute) const noexcept;
    /// The attribute's value, or `fallback` when absent.
    std::string attribute(std::string_view attribute, const std::string& fallback = {}) const;

    /// `text` with surrounding whitespace removed.
    std::string trimmed_text() const;
    /// Trimmed text of the first child with this name, or `fallback`.
    std::string child_text(std::string_view child_name, const std::string& fallback = {}) const;
};

struct ParseResult {
    bool ok = false;
    std::string error;  ///< "line 12: mismatched end tag </note>, expected </pitch>".
    int error_line = 0;
    Element root;
};

/// Parses a whole document. Never throws for malformed input.
ParseResult parse(std::string_view document);

/// Escapes text for element content (&, <, >) and, with `attribute`, for a
/// double-quoted attribute value as well. Characters XML 1.0 cannot carry
/// (C0 controls other than tab, newline and carriage return) are dropped.
std::string escape(std::string_view text, bool attribute = false);

using Attributes = std::vector<std::pair<std::string, std::string>>;

/// Streams an indented document. Elements are closed in reverse order of
/// opening; `finish()` closes whatever is still open.
class Writer {
public:
    explicit Writer(int indent = 2) : indent_(indent) {}

    /// `<?xml version="1.0" encoding="UTF-8" standalone="no"?>`
    void declaration();
    /// Writes a raw line (a DOCTYPE, for instance) verbatim.
    void raw_line(std::string_view line);

    void open(std::string_view name, const Attributes& attributes = {});
    void close();
    /// `<name attrs>text</name>`
    void leaf(std::string_view name, std::string_view text, const Attributes& attributes = {});
    /// `<name attrs/>`
    void empty(std::string_view name, const Attributes& attributes = {});

    /// Closes every open element and returns the document.
    std::string finish();

private:
    void start_line();
    void write_start(std::string_view name, const Attributes& attributes);

    std::string out_;
    std::vector<std::string> open_;
    int indent_;
};

}  // namespace vcc::interchange::xml
