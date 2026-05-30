#include "pdf_overlay_engine.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <iostream>

namespace PDFOverlay {

    // Helper to escape PDF special characters inside strings: '(', ')', and '\'
    static std::string escapePDFString(const std::string& input) {
        std::string result;
        result.reserve(input.size() * 1.1);
        for (char c : input) {
            if (c == '(' || c == ')' || c == '\\') {
                result.push_back('\\');
            }
            result.push_back(c);
        }
        return result;
    }

    bool Engine::parseDOMData(const std::string& dataFilePath, std::vector<Page>& outPages) {
        std::ifstream file(dataFilePath);
        if (!file.is_open()) {
            std::cerr << "[PDFEngine] Error: Cannot open input data file: " << dataFilePath << std::endl;
            return false;
        }

        std::string line;
        while (std::getline(file, line)) {
            // Trim whitespace
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
                line.pop_back();
            }
            if (line.empty()) continue;

            if (line.rfind("PAGE:", 0) == 0) {
                Page page;
                page.pageIndex = static_cast<int>(outPages.size()) + 1;
                
                // Parse: width, height
                size_t comma = line.find(',');
                if (comma != std::string::npos) {
                    try {
                        page.width = std::stof(line.substr(5, comma - 5));
                        page.height = std::stof(line.substr(comma + 1));
                    } catch (...) {
                        page.width = 595.0f;
                        page.height = 842.0f;
                    }
                }
                outPages.push_back(page);
            } 
            else if (line.rfind("BLOCK:", 0) == 0 && !outPages.empty()) {
                TextBlock block;
                size_t pipe = line.find('|');
                if (pipe != std::string::npos) {
                    block.text = line.substr(pipe + 1);
                    std::string nums = line.substr(6, pipe - 6);
                    std::vector<float> val;
                    std::string token;
                    std::stringstream ss(nums);
                    while (std::getline(ss, token, ',')) {
                        try {
                            val.push_back(std::stof(token));
                        } catch (...) {
                            val.push_back(0.0f);
                        }
                    }
                    if (val.size() >= 10) {
                        block.left = val[0];
                        block.top = val[1];
                        block.width = val[2];
                        block.height = val[3];
                        block.fontSize = val[4];
                        block.r = val[5];
                        block.g = val[6];
                        block.b = val[7];
                        block.scaleX = val[8];
                        block.scaleY = val[9];
                    }
                    outPages.back().blocks.push_back(block);
                }
            }
        }
        return true;
    }

    bool Engine::writePDF(const std::vector<Page>& pages, const std::string& outputPDFPath) {
        std::ofstream file(outputPDFPath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[PDFEngine] Error: Cannot write to output PDF file: " << outputPDFPath << std::endl;
            return false;
        }

        // 1. Write PDF Header
        file << "%PDF-1.4\n";
        // Binary marker to prevent text editors from converting newlines
        file << "%\xE2\xE3\xCF\xD3\n";

        // Keep track of byte offsets of all objects for the cross-reference table (xref)
        std::vector<size_t> offsets;
        // Reserve index 0 (unused)
        offsets.push_back(0);

        auto start_obj = [&](int id) {
            if (id >= static_cast<int>(offsets.size())) {
                offsets.resize(id + 1, 0);
            }
            offsets[id] = file.tellp();
            file << id << " 0 obj\n";
        };

        auto end_obj = [&]() {
            file << "endobj\n";
        };

        // 2. Object 1: Catalog
        start_obj(1);
        file << "<< /Type /Catalog /Pages 2 0 R >>\n";
        end_obj();

        // 3. Object 2: Pages Tree
        int numPages = static_cast<int>(pages.size());
        start_obj(2);
        file << "<< /Type /Pages /Kids [ ";
        for (int i = 0; i < numPages; ++i) {
            file << (4 + 2 * i) << " 0 R ";
        }
        file << "] /Count " << numPages << " >>\n";
        end_obj();

        // 4. Object 3: Default shared standard Helvetica Font
        start_obj(3);
        file << "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\n";
        end_obj();

        // 5. Generate Pages & Contents Objects
        for (int i = 0; i < numPages; ++i) {
            const auto& page = pages[i];
            int pageObjId = 4 + 2 * i;
            int contentObjId = 5 + 2 * i;

            // Page Object
            start_obj(pageObjId);
            file << "<< /Type /Page\n";
            file << "   /Parent 2 0 R\n";
            file << "   /Resources <<\n";
            file << "     /Font << /F1 3 0 R >>\n";
            file << "   >>\n";
            file << "   /MediaBox [ 0 0 " << page.width << " " << page.height << " ]\n";
            file << "   /Contents " << contentObjId << " 0 R\n";
            file << ">>\n";
            end_obj();

            // Construct Content Stream data
            std::stringstream streamData;
            streamData << "BT\n"; // Begin Text

            for (const auto& block : page.blocks) {
                // Skip empty blocks
                if (block.text.empty()) continue;

                // Escape text strings safely
                std::string escaped = escapePDFString(block.text);

                // Set font and size
                streamData << "/F1 " << block.fontSize << " Tf\n";

                // Set non-stroking (fill) color in RGB space
                streamData << block.r << " " << block.g << " " << block.b << " rg\n";

                // Map absolute DOM coordinates to PDF coordinates
                // PDF bottom-left is (0,0) -> CSS top-left is (0,0)
                // Baseline offset: Y coordinate represents the baseline of the text.
                float pdfX = block.left;
                float pdfY = page.height - block.top - block.fontSize;

                // Set text matrix (Tm): combines scaling (scaleX, scaleY) and position (pdfX, pdfY)
                // Tm operator: a b c d e f Tm
                // [ scaleX    0      0 ]
                // [   0     scaleY   0 ]
                // [  pdfX    pdfY    1 ]
                streamData << block.scaleX << " 0 0 " << block.scaleY << " " << pdfX << " " << pdfY << " Tm\n";

                // Output text string object
                streamData << "(" << escaped << ") Tj\n";
            }

            streamData << "ET\n"; // End Text

            std::string contentStr = streamData.str();

            // Content Stream Object
            start_obj(contentObjId);
            file << "<< /Length " << contentStr.size() << " >>\n";
            file << "stream\n";
            file << contentStr;
            file << "endstream\n";
            end_obj();
        }

        // 6. Write Cross-Reference (xref) Table
        size_t xrefOffset = file.tellp();
        int totalObjects = static_cast<int>(offsets.size()) - 1;
        file << "xref\n";
        file << "0 " << (totalObjects + 1) << "\n";
        file << "0000000000 65535 f \n"; // entry for object 0

        for (int i = 1; i <= totalObjects; ++i) {
            // Write zero-padded 10-digit offsets
            file << std::setw(10) << std::setfill('0') << offsets[i] << " 00000 n \n";
        }

        // 7. Write Trailer & EOF
        file << "trailer\n";
        file << "<< /Size " << (totalObjects + 1) << "\n";
        file << "   /Root 1 0 R\n";
        file << ">>\n";
        file << "startxref\n";
        file << xrefOffset << "\n";
        file << "%%EOF\n";

        file.close();
        return true;
    }

    bool Engine::renderDOMToPDF(const std::string& inputDataPath, const std::string& outputPDFPath) {
        std::vector<Page> pages;
        if (!parseDOMData(inputDataPath, pages)) {
            std::cerr << "[PDFEngine] Failed to parse input DOM data file." << std::endl;
            return false;
        }
        if (pages.empty()) {
            std::cerr << "[PDFEngine] Warning: No pages parsed from DOM data file." << std::endl;
            return false;
        }
        return writePDF(pages, outputPDFPath);
    }

} // namespace PDFOverlay
