#include <iostream>
#include <fstream>
#include <string>
#include "export.hpp"
#include "pdf_overlay_engine.hpp"

using namespace std;

bool exportDocument::is_pdf(string file_path) {
    ifstream file(file_path, ios::binary);
    if (file.is_open()) {
        char signature[5];
        file.read(signature, 4);
        signature[4] = '\0';
        return string(signature) == "%PDF";
    }
    return false;
}

bool exportDocument::render_dom_to_pdf(const string& input_data_path, const string& output_pdf_path) {
    PDFOverlay::Engine engine;
    return engine.renderDOMToPDF(input_data_path, output_pdf_path);
}