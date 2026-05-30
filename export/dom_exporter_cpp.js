/**
 * ALGORITHM: FRONTEND TO STANDALONE C++ LAYOUT BRIDGE
 * 
 * This Javascript function serializes the active browser DOM `.bbox-translated` elements
 * directly into the lightweight custom data format parsed by our standalone C++ PDF engine.
 * 
 * Output format:
 * PAGE: width, height
 * BLOCK: left, top, width, height, fontSize, r, g, b, scaleX, scaleY | text
 * ...
 */

function serializeDOMForCPPEngine(numPages) {
    const lines = [];

    // Helper to parse CSS color string "rgb(r, g, b)" into normalized float components [0.0, 1.0]
    function parseRGBToFloat(colorStr) {
        const rgbMatch = colorStr.match(/rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)/i);
        if (rgbMatch) {
            return {
                r: (parseInt(rgbMatch[1]) / 255.0).toFixed(3),
                g: (parseInt(rgbMatch[2]) / 255.0).toFixed(3),
                b: (parseInt(rgbMatch[3]) / 255.0).toFixed(3)
            };
        }
        // Fallback to black if parsing fails
        return { r: "0.000", g: "0.000", b: "0.000" };
    }

    // Helper to extract horizontal scale factor (scaleX) from transform matrix or inline style
    function getScaleFactors(element) {
        const transform = element.style.transform || "";
        let scaleX = 1.0;
        let scaleY = 1.0;

        // Try matching scaleX(value) or scale(value)
        const scaleMatch = transform.match(/scaleX\(\s*([\d.]+)\s*\)/i) || transform.match(/scale\(\s*([\d.]+)\s*\)/i);
        if (scaleMatch) {
            scaleX = parseFloat(scaleMatch[1]);
        }
        // Try matching matrix(a, b, c, d, tx, ty) where a is scaleX and d is scaleY
        const matrixMatch = transform.match(/matrix\(\s*([\d.-]+)\s*,\s*[\d.-]+\s*,\s*[\d.-]+\s*,\s*([\d.-]+)\s*,/i);
        if (matrixMatch) {
            scaleX = parseFloat(matrixMatch[1]);
            scaleY = parseFloat(matrixMatch[2]);
        }

        return {
            scaleX: scaleX.toFixed(3),
            scaleY: scaleY.toFixed(3)
        };
    }

    for (let i = 1; i <= numPages; i++) {
        const pageNode = document.getElementById(`trans-page-${i}`);
        if (!pageNode) continue;

        const pageW = parseFloat(pageNode.dataset.logicalWidth || pageNode.style.width) || 595.0;
        const pageH = parseFloat(pageNode.dataset.logicalHeight || pageNode.style.height) || 842.0;

        // Add page marker line
        lines.push(`PAGE: ${pageW.toFixed(1)}, ${pageH.toFixed(1)}`);

        // Query all absolute translated text box nodes
        const allDivs = pageNode.querySelectorAll('.bbox-translated');
        allDivs.forEach((div) => {
            if (div.style.visibility === 'hidden') return;

            const left = parseFloat(div.style.left) || 0.0;
            const top = parseFloat(div.style.top) || 0.0;
            const width = parseFloat(div.style.width) || 0.0;
            const height = parseFloat(div.style.height) || 12.0;
            const fontSize = parseFloat(div.style.fontSize) || 12.0;

            // Extract and normalize color
            const color = window.getComputedStyle(div).color || "rgb(0,0,0)";
            const rgb = parseRGBToFloat(color);

            // Extract CSS transformation scale
            const scale = getScaleFactors(div);

            // Extract text and strip newlines/carriage returns
            const text = (div.textContent || "").replace(/[\r\n]+/g, " ").trim();

            if (text.length > 0) {
                // Add structured block line
                // Format: BLOCK: left,top,width,height,fontSize,r,g,b,scaleX,scaleY|text
                lines.push(`BLOCK: ${left.toFixed(2)},${top.toFixed(2)},${width.toFixed(2)},${height.toFixed(2)},${fontSize.toFixed(2)},${rgb.r},${rgb.g},${rgb.b},${scale.scaleX},${scale.scaleY}|${text}`);
            }
        });
    }

    // Join with newline characters to build the final plain-text file content
    return lines.join("\n");
}
