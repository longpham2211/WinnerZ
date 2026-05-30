#include <iostream>
#include <fstream>
#include "export.hpp"

using namespace std;

void createMockDOMDataFile(const string& path) {
    ofstream file(path);
    if (!file.is_open()) return;

    // PAGE 1: Standard A4 (595 x 842)
    file << "PAGE: 595.0, 842.0\n";
    // Format: left, top, width, height, fontSize, r, g, b, scaleX, scaleY | text
    file << "BLOCK: 50.0, 50.0, 200.0, 24.0, 24.0, 0.1, 0.2, 0.6, 1.0, 1.0 | Google Deepmind's Antigravity Engine\n";
    file << "BLOCK: 50.0, 90.0, 400.0, 14.0, 12.0, 0.2, 0.2, 0.2, 1.0, 1.0 | This C++ engine renders high-fidelity DOM layouts directly to PDF with zero dependencies.\n";
    file << "BLOCK: 50.0, 120.0, 300.0, 16.0, 14.0, 0.8, 0.1, 0.1, 1.0, 1.0 | Pixel-Perfect Vector Layout (Red text)\n";
    // Text scaling/compression example: horizontal compression (scaleX = 0.7)
    file << "BLOCK: 50.0, 150.0, 150.0, 14.0, 12.0, 0.1, 0.5, 0.1, 0.7, 1.0 | Compressed Text Example (scaleX = 0.7)\n";
    // Text scaling/expansion example: horizontal expansion (scaleX = 1.3)
    file << "BLOCK: 50.0, 180.0, 250.0, 14.0, 12.0, 0.4, 0.4, 0.0, 1.3, 1.0 | Expanded Text Example (scaleX = 1.3)\n";

    // PAGE 2: Landscape Page (842 x 595)
    file << "PAGE: 842.0, 595.0\n";
    file << "BLOCK: 100.0, 80.0, 400.0, 32.0, 32.0, 0.3, 0.0, 0.5, 1.0, 1.0 | Page 2: Landscape Document Layout\n";
    file << "BLOCK: 100.0, 150.0, 500.0, 14.0, 12.0, 0.3, 0.3, 0.3, 1.0, 1.0 | Fully scalable vector typography drawn using PDF standard fonts natively.\n";

    file.close();
    cout << "[Main] Generated mock layout file: " << path << endl;
}

int main() {
    string layoutDataPath = "mock_layout.txt";
    string outputPDFPath = "final_output.pdf";

    cout << "==================================================" << endl;
    cout << "  Zero-Dependency DOM-to-PDF C++ Rendering Engine  " << endl;
    cout << "==================================================" << endl;

    // 1. Generate local mock DOM coordinate data
    createMockDOMDataFile(layoutDataPath);

    // 2. Instantiate and run our layout-rendering export class
    exportDocument docExporter;
    cout << "[Main] Initiating DOM-to-PDF layout rendering..." << endl;
    bool success = docExporter.render_dom_to_pdf(layoutDataPath, outputPDFPath);

    if (success) {
        cout << "[Main] SUCCESS: Generated vector PDF: " << outputPDFPath << endl;

        // 3. Validate generated file signature using is_pdf signature
        if (docExporter.is_pdf(outputPDFPath)) {
            cout << "[Main] SUCCESS: Signature check PASSED! Valid %PDF file detected." << endl;
        } else {
            cout << "[Main] ERROR: Signature check FAILED!" << endl;
        }
    } else {
        cout << "[Main] ERROR: Failed to render DOM to PDF." << endl;
    }

    return success ? 0 : 1;
}
