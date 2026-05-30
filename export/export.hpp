#pragma once
#include <string>
using namespace std;

class exportDocument {
private:
    string file_path;
public:
    exportDocument() = default;
    ~exportDocument() = default;

    // Checks if the given file starts with standard PDF signature
    bool is_pdf(string file_path);

    // Renders custom structured DOM layout file directly to output PDF with zero external libraries
    bool render_dom_to_pdf(const string& input_data_path, const string& output_pdf_path);
};