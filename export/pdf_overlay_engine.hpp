#pragma once
#include <string>
#include <vector>

namespace PDFOverlay {

    // Structure representing a single translated DOM text element
    struct TextBlock {
        std::string text;
        float left = 0.0f;
        float top = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float fontSize = 12.0f;
        float r = 0.0f; // Red color component (0.0 to 1.0)
        float g = 0.0f; // Green color component (0.0 to 1.0)
        float b = 0.0f; // Blue color component (0.0 to 1.0)
        float scaleX = 1.0f; // CSS scaleX transform
        float scaleY = 1.0f; // CSS scaleY transform
    };

    // Structure representing a single page matching the logical DOM page
    struct Page {
        int pageIndex = 0;
        float width = 595.0f;  // Default A4 width
        float height = 842.0f; // Default A4 height
        std::vector<TextBlock> blocks;
    };

    class Engine {
    public:
        Engine() = default;
        ~Engine() = default;

        // Parses the custom high-fidelity layout data format exported from DOM
        bool parseDOMData(const std::string& dataFilePath, std::vector<Page>& outPages);

        // Generates a fully-compliant PDF from scratch with zero calculations using Standard Fonts (Helvetica)
        bool writePDF(const std::vector<Page>& pages, const std::string& outputPDFPath);

        // Convenient single-call handler
        bool renderDOMToPDF(const std::string& inputDataPath, const std::string& outputPDFPath);
    };

} // namespace PDFOverlay
