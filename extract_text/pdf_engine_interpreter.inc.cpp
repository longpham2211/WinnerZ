void WinPdfInterpreter::run(const std::vector<uint8_t>& stream,
                           WinTextExtractor& extractor,
                           const WinFontUnicodeMap& font_unicode_map,
                           const WinFontWidthMap& font_width_map,
                           const WinFontCodeBytesMap& font_code_bytes_map,
                           const WinFontCodeSpaceMap& font_codespace_map,
                           const WinFontMatrixMap& font_matrix_map,
                           const WinFontVerticalMetricsMap& font_vertical_metrics_map,
                           const WinFontW2Map& font_w2_map,
                           const WinColorSpaceMap& color_space_map,
                           std::shared_ptr<const WinFormXObjectMap> form_xobject_map,
                           std::shared_ptr<const WinImageXObjectMap> image_xobject_map,
                           const float* initial_ctm,
                           int recursion_depth,
                           const Rect* page_mediabox,
                           const Rect* inherited_clip_box,
                           const uint32_t* inherited_fill_color,
                           const uint32_t* inherited_stroke_color,
                           const WinColorSpaceDef* inherited_fill_space,
                           const WinColorSpaceDef* inherited_stroke_space,
                           const int* inherited_text_render_mode) {
    if (recursion_depth > 32) {
        return;
    }

    thread_local int global_run_count = 0;
    if (recursion_depth == 0) {
        global_run_count = 0;
    }
    if (++global_run_count > 2000) {
        return;
    }

    TextState st;
    std::vector<PdfToken> operands;
    bool in_text_object = false;
    float ctm[6] = {1, 0, 0, 1, 0, 0};
    if (initial_ctm) {
        copy_matrix(ctm, initial_ctm);
    }
    struct GraphicsStateSnapshot {
        std::array<float, 6> ctm;
        bool has_clip = false;
        Rect clip = {0, 0, 0, 0};
        TextState text_state;
        const std::unordered_map<int, std::vector<int>>* font_map = nullptr;
        const std::unordered_map<int, float>* width_map = nullptr;
        const WinW2MetricsMap* w2_map = nullptr;
        const std::vector<WinCodeSpaceRange>* codespace_ranges = nullptr;
        int code_bytes = 1;
        float default_advance = 0.55f;
        bool is_italic = false;
        bool is_bold = false;
        float ascender = 0.8f;
        float descender = -0.2f;
        int bidi = 0;
        uint32_t fill_color = 0;
        uint32_t stroke_color = 0;
        WinColorSpaceDef fill_space;
        WinColorSpaceDef stroke_space;
        int render_mode = 0;
    };
    std::vector<GraphicsStateSnapshot> gstate_stack;
    const std::unordered_map<int, std::vector<int>>* active_font_map = nullptr;
    const std::unordered_map<int, float>* active_width_map = nullptr;
    const WinW2MetricsMap* active_w2_map = nullptr;
    const std::vector<WinCodeSpaceRange>* active_codespace_ranges = nullptr;
    int active_code_bytes = 1;
    float active_default_advance = 0.55f;
    bool active_is_italic = false;
    bool active_is_bold = false;
    bool active_is_serif = true;
    bool active_is_mono = false;
    float active_ascender = 0.8f;
    float active_descender = -0.2f;
    int active_bidi = 0;
    uint32_t current_fill_color = 0;
    uint32_t current_stroke_color = 0;
    WinColorSpaceDef current_fill_space = make_colorspace_def(WinColorSpaceKind::DeviceGray, 1);
    WinColorSpaceDef current_stroke_space = make_colorspace_def(WinColorSpaceKind::DeviceGray, 1);
    int text_render_mode = 0;

    if (inherited_fill_color != nullptr) {
        current_fill_color = *inherited_fill_color;
    }
    if (inherited_stroke_color != nullptr) {
        current_stroke_color = *inherited_stroke_color;
    }
    if (inherited_fill_space != nullptr) {
        current_fill_space = *inherited_fill_space;
    }
    if (inherited_stroke_space != nullptr) {
        current_stroke_space = *inherited_stroke_space;
    }
    if (inherited_text_render_mode != nullptr) {
        text_render_mode = *inherited_text_render_mode;
        if (text_render_mode < 0) text_render_mode = 0;
        if (text_render_mode > 7) text_render_mode = 7;
    }

    bool clip_has_box = false;
    Rect current_clip_box = {0, 0, 0, 0};
    if (page_mediabox != nullptr) {
        clip_has_box = true;
        current_clip_box = *page_mediabox;
    }
    if (inherited_clip_box != nullptr) {
        if (!clip_has_box) {
            current_clip_box = *inherited_clip_box;
            clip_has_box = true;
        } else {
            current_clip_box.x0 = std::max(current_clip_box.x0, inherited_clip_box->x0);
            current_clip_box.y0 = std::max(current_clip_box.y0, inherited_clip_box->y0);
            current_clip_box.x1 = std::min(current_clip_box.x1, inherited_clip_box->x1);
            current_clip_box.y1 = std::min(current_clip_box.y1, inherited_clip_box->y1);
        }
    }

    bool pending_clip = false;
    bool path_has_bbox = false;
    Rect path_bbox = {0, 0, 0, 0};
    Vec2 path_current = {0.0f, 0.0f};
    Vec2 path_subpath_start = {0.0f, 0.0f};
    bool path_has_current = false;

    auto add_path_point = [&](const Vec2& pt) {
        if (!path_has_bbox) {
            path_bbox = {pt.x, pt.y, pt.x, pt.y};
            path_has_bbox = true;
            return;
        }
        path_bbox.x0 = std::min(path_bbox.x0, pt.x);
        path_bbox.y0 = std::min(path_bbox.y0, pt.y);
        path_bbox.x1 = std::max(path_bbox.x1, pt.x);
        path_bbox.y1 = std::max(path_bbox.y1, pt.y);
    };

    auto reset_current_path = [&]() {
        path_has_bbox = false;
        path_bbox = {0, 0, 0, 0};
        path_has_current = false;
        path_current = {0.0f, 0.0f};
        path_subpath_start = {0.0f, 0.0f};
    };

    auto consume_current_path = [&]() {
        if (pending_clip && path_has_bbox) {
            if (!clip_has_box) {
                current_clip_box = path_bbox;
                clip_has_box = true;
            } else {
                current_clip_box.x0 = std::max(current_clip_box.x0, path_bbox.x0);
                current_clip_box.y0 = std::max(current_clip_box.y0, path_bbox.y0);
                current_clip_box.x1 = std::min(current_clip_box.x1, path_bbox.x1);
                current_clip_box.y1 = std::min(current_clip_box.y1, path_bbox.y1);
            }
        }
        pending_clip = false;
        reset_current_path();
    };

    auto glyph_entirely_outside_box = [&](const float m[6], float adv, int wmode, float ascender, float descender, const Rect& box) -> bool {
        const float det = m[0] * m[3] - m[1] * m[2];
        const float m_size = std::sqrt(std::abs(det));
        if (!(m_size > 0.0f)) {
            return false;
        }

        Vec2 dir;
        if (wmode == 0) {
            dir = {m[0], m[1]};
        } else {
            dir = {-m[2], -m[3]};
        }

        const float dir_len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (!(dir_len > 0.0f)) {
            return false;
        }

        Vec2 ndir = {dir.x / dir_len, dir.y / dir_len};
        Vec2 p;
        Vec2 q;
        if (wmode == 0) {
            p = {m[4], m[5]};
            q = {m[4] + adv * dir.x, m[5] + adv * dir.y};
        } else {
            p = {m[4] - adv * dir.x, m[5] - adv * dir.y};
            q = {m[4], m[5]};
        }

        float asc = ascender;
        float dsc = -descender;
        if (!std::isfinite(asc) || asc <= 0.01f) {
            asc = 0.8f;
        }
        if (!std::isfinite(dsc) || dsc <= 0.01f) {
            dsc = 0.2f;
        }

        Vec2 up = {-ndir.y * m_size, ndir.x * m_size};

        Vec2 ll = {p.x - up.x * dsc, p.y - up.y * dsc};
        Vec2 ul = {p.x + up.x * asc, p.y + up.y * asc};
        Vec2 lr = {q.x - up.x * dsc, q.y - up.y * dsc};
        Vec2 ur = {q.x + up.x * asc, q.y + up.y * asc};

        Rect glyph_rect = {
            std::min({ll.x, ul.x, lr.x, ur.x}),
            std::min({ll.y, ul.y, lr.y, ur.y}),
            std::max({ll.x, ul.x, lr.x, ur.x}),
            std::max({ll.y, ul.y, lr.y, ur.y})
        };

        return glyph_rect.x1 <= box.x0 ||
               glyph_rect.y1 <= box.y0 ||
               glyph_rect.x0 >= box.x1 ||
               glyph_rect.y0 >= box.y1;
    };

    struct ActualTextState {
        std::vector<int> text;
    };
    std::vector<ActualTextState> actual_text_stack;

    auto active_actualtext = [&]() -> ActualTextState* {
        for (auto it = actual_text_stack.rbegin(); it != actual_text_stack.rend(); ++it) {
            if (!it->text.empty()) {
                return &(*it);
            }
        }
        return nullptr;
    };

    auto decode_actualtext_from_operands = [&](const std::vector<PdfToken>& op_tokens) -> std::vector<int> {
        for (auto it = op_tokens.rbegin(); it != op_tokens.rend(); ++it) {
            if (it->type != PdfToken::Type::Dictionary || it->dict.empty()) {
                continue;
            }

            std::string actual_utf8;
            if (extract_actual_text_from_dict(it->dict, actual_utf8)) {
                return decode_utf8_to_codepoints(actual_utf8);
            }
        }
        return {};
    };

    auto clamp01 = [](float v) -> float {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    };

    auto to_u8 = [&](float v) -> uint32_t {
        const float c = clamp01(v);
        return static_cast<uint32_t>(std::lround(c * 255.0f));
    };

    auto rgb_from_gray = [&](float g) -> uint32_t {
        const uint32_t gv = to_u8(g);
        return (gv << 16) | (gv << 8) | gv;
    };

    auto rgb_from_cmyk = [&](float c, float m, float y, float k) -> uint32_t {
        const float cc = clamp01(c);
        const float mm = clamp01(m);
        const float yy = clamp01(y);
        const float kk = clamp01(k);

#if defined(WINNERZ_USE_LCMS2) && WINNERZ_USE_LCMS2
        float ir = 0.0f;
        float ig = 0.0f;
        float ib = 0.0f;
        if (win_icc_convert_cmyk_to_rgb(cc, mm, yy, kk, ir, ig, ib)) {
            return (to_u8(ir) << 16) | (to_u8(ig) << 8) | to_u8(ib);
        }
#endif

        // not absolute RGB 0. Keep that anchor for parity in Separation /Black flows.
        if (cc <= 1e-6f && mm <= 1e-6f && yy <= 1e-6f && kk >= 0.999f) {
            return (35u << 16) | (31u << 8) | 32u; 
        }

        const float r = 1.0f - std::min(1.0f, cc + kk);
        const float g = 1.0f - std::min(1.0f, mm + kk);
        const float b = 1.0f - std::min(1.0f, yy + kk);
        return (to_u8(r) << 16) | (to_u8(g) << 8) | to_u8(b);
    };

    auto rgb_from_lab = [&](float l, float a, float b, const WinColorSpaceDef* space_def) -> uint32_t {
        float xw = 0.9642f;
        float yw = 1.0000f;
        float zw = 0.8249f;
        if (space_def != nullptr &&
            space_def->lab_white_x > 0.0f &&
            space_def->lab_white_y > 0.0f &&
            space_def->lab_white_z > 0.0f) {
            xw = space_def->lab_white_x;
            yw = space_def->lab_white_y;
            zw = space_def->lab_white_z;
        }

        const float ll = std::max(0.0f, std::min(100.0f, l));
        const float aa = std::max(-128.0f, std::min(127.0f, a));
        const float bb = std::max(-128.0f, std::min(127.0f, b));

#if defined(WINNERZ_USE_LCMS2) && WINNERZ_USE_LCMS2
        if (std::abs(xw - 0.964203f) <= 1e-4f &&
            std::abs(yw - 1.000000f) <= 1e-4f &&
            std::abs(zw - 0.824905f) <= 1e-4f) {
            float ir = 0.0f;
            float ig = 0.0f;
            float ib = 0.0f;
            if (win_icc_convert_lab_to_rgb(ll, aa, bb, ir, ig, ib)) {
                return (to_u8(ir) << 16) | (to_u8(ig) << 8) | to_u8(ib);
            }
        }
#endif

        const float fy = (ll + 16.0f) / 116.0f;
        const float fx = fy + (aa / 500.0f);
        const float fz = fy - (bb / 200.0f);

        auto lab_inv = [](float t) -> float {
            constexpr float eps = 216.0f / 24389.0f;
            constexpr float kappa = 24389.0f / 27.0f;
            const float t3 = t * t * t;
            if (t3 > eps) {
                return t3;
            }
            return (116.0f * t - 16.0f) / kappa;
        };

        const float X = xw * lab_inv(fx);
        const float Y = yw * lab_inv(fy);
        const float Z = zw * lab_inv(fz);

        // D50 XYZ to linear sRGB.
        float r_lin =  3.1338561f * X - 1.6168667f * Y - 0.4906146f * Z;
        float g_lin = -0.9787684f * X + 1.9161415f * Y + 0.0334540f * Z;
        float b_lin =  0.0719453f * X - 0.2289914f * Y + 1.4052427f * Z;

        auto gamma_encode = [](float v) -> float {
            if (v <= 0.0f) {
                return 0.0f;
            }
            if (v < 0.0031308f) {
                return 12.92f * v;
            }
            return 1.055f * static_cast<float>(std::pow(static_cast<double>(v), 1.0 / 2.4)) - 0.055f;
        };

        r_lin = gamma_encode(r_lin);
        g_lin = gamma_encode(g_lin);
        b_lin = gamma_encode(b_lin);
        return (to_u8(r_lin) << 16) | (to_u8(g_lin) << 8) | to_u8(b_lin);
    };

    auto eval_icc_curve = [&](const WinIccCurve& curve, float x) -> float {
        const float in = clamp01(x);
        switch (curve.type) {
            case WinIccCurveType::Identity:
                return in;
            case WinIccCurveType::Gamma: {
                const float g = (curve.gamma > 0.0f) ? curve.gamma : 1.0f;
                return clamp01(static_cast<float>(std::pow(static_cast<double>(in), static_cast<double>(g))));
            }
            case WinIccCurveType::Table: {
                if (curve.table.empty()) {
                    return in;
                }
                if (curve.table.size() == 1) {
                    return clamp01(curve.table[0]);
                }
                const float pos = in * static_cast<float>(curve.table.size() - 1);
                const size_t i0 = static_cast<size_t>(std::floor(pos));
                const size_t i1 = std::min(i0 + 1, curve.table.size() - 1);
                const float frac = pos - static_cast<float>(i0);
                const float v0 = curve.table[i0];
                const float v1 = curve.table[i1];
                return clamp01(v0 + (v1 - v0) * frac);
            }
            default:
                return in;
        }
    };

    auto rgb_from_icc_profile = [&](const WinColorSpaceDef* space_def, float r, float g, float b) -> uint32_t {
        if (space_def == nullptr || !space_def->has_icc_rgb_profile) {
            return (to_u8(r) << 16) | (to_u8(g) << 8) | to_u8(b);
        }

        const float rr = eval_icc_curve(space_def->icc_trc_r, r);
        const float gg = eval_icc_curve(space_def->icc_trc_g, g);
        const float bb = eval_icc_curve(space_def->icc_trc_b, b);

        const auto& M = space_def->icc_rgb_to_xyz;
        const float X = M[0] * rr + M[1] * gg + M[2] * bb;
        const float Y = M[3] * rr + M[4] * gg + M[5] * bb;
        const float Z = M[6] * rr + M[7] * gg + M[8] * bb;

        float r_lin =  3.1338561f * X - 1.6168667f * Y - 0.4906146f * Z;
        float g_lin = -0.9787684f * X + 1.9161415f * Y + 0.0334540f * Z;
        float b_lin =  0.0719453f * X - 0.2289914f * Y + 1.4052427f * Z;

        auto gamma_encode = [](float v) -> float {
            if (v <= 0.0f) {
                return 0.0f;
            }
            if (v < 0.0031308f) {
                return 12.92f * v;
            }
            return 1.055f * static_cast<float>(std::pow(static_cast<double>(v), 1.0 / 2.4)) - 0.055f;
        };

        r_lin = gamma_encode(r_lin);
        g_lin = gamma_encode(g_lin);
        b_lin = gamma_encode(b_lin);

        return (to_u8(r_lin) << 16) | (to_u8(g_lin) << 8) | to_u8(b_lin);
    };

    auto set_color_from_components = [&](WinColorSpaceKind kind,
                                         const std::vector<float>& comps,
                                         const WinColorSpaceDef* space_def,
                                         uint32_t& out_color) -> bool {
        switch (kind) {
            case WinColorSpaceKind::DeviceGray:
                if (!comps.empty()) {
                    out_color = rgb_from_gray(comps[0]);
                    return true;
                }
                return false;
            case WinColorSpaceKind::DeviceRGB:
                if (comps.size() >= 3) {
                    out_color = (to_u8(comps[0]) << 16) | (to_u8(comps[1]) << 8) | to_u8(comps[2]);
                    return true;
                }
                if (!comps.empty()) {
                    out_color = rgb_from_gray(comps[0]);
                    return true;
                }
                return false;
            case WinColorSpaceKind::DeviceCMYK:
                if (comps.size() >= 4) {
                    out_color = rgb_from_cmyk(comps[0], comps[1], comps[2], comps[3]);
                    return true;
                }
                if (comps.size() >= 3) {
                    out_color = (to_u8(comps[0]) << 16) | (to_u8(comps[1]) << 8) | to_u8(comps[2]);
                    return true;
                }
                if (!comps.empty()) {
                    out_color = rgb_from_gray(comps[0]);
                    return true;
                }
                return false;
            case WinColorSpaceKind::Lab:
                if (comps.size() >= 3) {
                    out_color = rgb_from_lab(comps[0], comps[1], comps[2], space_def);
                    return true;
                }
                return false;
            case WinColorSpaceKind::ICCBased:
                if (space_def != nullptr && space_def->has_icc_rgb_profile && comps.size() >= 3) {
                    out_color = rgb_from_icc_profile(space_def, comps[0], comps[1], comps[2]);
                    return true;
                }
                if (space_def != nullptr) {
                    if (space_def->component_count == 1 && !comps.empty()) {
                        out_color = rgb_from_gray(comps[0]);
                        return true;
                    }
                    if (space_def->component_count == 3 && comps.size() >= 3) {
                        out_color = (to_u8(comps[0]) << 16) | (to_u8(comps[1]) << 8) | to_u8(comps[2]);
                        return true;
                    }
                    if (space_def->component_count == 4 && comps.size() >= 4) {
                        out_color = rgb_from_cmyk(comps[0], comps[1], comps[2], comps[3]);
                        return true;
                    }
                }
                return false;
            default:
                break;
        }
        return false;
    };

    auto resolve_named_color_space = [&](const std::string& name,
                                         const WinColorSpaceDef& fallback) -> WinColorSpaceDef {
        WinColorSpaceDef direct = colorspace_from_name(name);
        if (direct.kind != WinColorSpaceKind::Unknown) {
            return direct;
        }

        auto it = color_space_map.find(name);
        if (it != color_space_map.end()) {
            return it->second;
        }

        return fallback;
    };

    auto apply_color_operands = [&](const WinColorSpaceDef& active_space,
                                    const std::vector<PdfToken>& op_tokens,
                                    uint32_t& out_color) -> bool {
        std::vector<float> comps;
        comps.reserve(op_tokens.size());
        for (const auto& tok : op_tokens) {
            if (tok.type == PdfToken::Type::Number) {
                comps.push_back(static_cast<float>(tok.number));
            }
        }
        if (comps.empty()) {
            return false;
        }

        WinColorSpaceKind kind = active_space.kind;
        if (kind == WinColorSpaceKind::Pattern && active_space.alt_kind != WinColorSpaceKind::Unknown) {
            kind = active_space.alt_kind;
        }

        if (kind == WinColorSpaceKind::ICCBased &&
            !(active_space.has_icc_rgb_profile && active_space.component_count == 3)) {
            if (active_space.component_count == 1) {
                kind = WinColorSpaceKind::DeviceGray;
            } else if (active_space.component_count == 3) {
                kind = WinColorSpaceKind::DeviceRGB;
            } else if (active_space.component_count == 4) {
                kind = WinColorSpaceKind::DeviceCMYK;
            }
        }

        if (kind == WinColorSpaceKind::Separation || kind == WinColorSpaceKind::DeviceN) {
            bool tint_success = false;
            if (active_space.has_tint_transform) {
                std::vector<float> tint_values;
                if (evaluate_tint_transform(active_space, comps, tint_values) && !tint_values.empty()) {

                    WinColorSpaceKind tint_kind = active_space.alt_kind;
                    if (tint_kind == WinColorSpaceKind::Unknown) {
                        const int n = static_cast<int>(tint_values.size());
                        if (n == 1) {
                            tint_kind = WinColorSpaceKind::DeviceGray;
                        } else if (n == 3) {
                            tint_kind = WinColorSpaceKind::DeviceRGB;
                        } else if (n >= 4) {
                            tint_kind = WinColorSpaceKind::DeviceCMYK;
                        }
                    }

                    WinColorSpaceDef alt_space;
                    alt_space.kind = tint_kind;
                    alt_space.component_count = active_space.alt_component_count;
                    if (alt_space.component_count <= 0) {
                        alt_space.component_count = static_cast<int>(tint_values.size());
                    }

                    if (set_color_from_components(tint_kind, tint_values, &alt_space, out_color)) {
                        tint_success = true;
                        return true;
                    }
                    if (set_color_from_components(active_space.alt_kind, tint_values, &alt_space, out_color)) {
                        tint_success = true;
                        return true;
                    }
                }
            }

            if (!tint_success && !comps.empty()) {
                // Fallback if tint_transform is unsupported (e.g. Type 4) or missing
                if (active_space.alt_kind == WinColorSpaceKind::DeviceCMYK && comps.size() == 1) {
                    std::vector<float> tint_values = {0.0f, 0.0f, 0.0f, comps[0]};
                    if (set_color_from_components(active_space.alt_kind, tint_values, &active_space, out_color)) {
                        return true;
                    }
                } else if (active_space.alt_kind == WinColorSpaceKind::DeviceGray && comps.size() == 1) {
                    std::vector<float> tint_values = {1.0f - comps[0]};
                    if (set_color_from_components(active_space.alt_kind, tint_values, &active_space, out_color)) {
                        return true;
                    }
                }
            }
        }

        if (kind == WinColorSpaceKind::Separation ||
            kind == WinColorSpaceKind::DeviceN ||
            kind == WinColorSpaceKind::Indexed) {
            if (active_space.alt_kind != WinColorSpaceKind::Unknown) {
                kind = active_space.alt_kind;
            }
        }

        if (set_color_from_components(kind, comps, &active_space, out_color)) {
            return true;
        }

        if (set_color_from_components(active_space.alt_kind, comps, &active_space, out_color)) {
            return true;
        }

        if (comps.size() >= 4) {
            return set_color_from_components(WinColorSpaceKind::DeviceCMYK, comps, &active_space, out_color);
        }
        if (comps.size() >= 3) {
            return set_color_from_components(WinColorSpaceKind::DeviceRGB, comps, &active_space, out_color);
        }
        return set_color_from_components(WinColorSpaceKind::DeviceGray, comps, &active_space, out_color);
    };

    auto guess_bidi_level = [](int bidiclass, int cur_bidi) -> int {
        switch (bidiclass) {
            case UCDN_BIDI_CLASS_L: return 0;
            case UCDN_BIDI_CLASS_R:
            case UCDN_BIDI_CLASS_AL: return 1;
            case UCDN_BIDI_CLASS_EN:
            case UCDN_BIDI_CLASS_ES:
            case UCDN_BIDI_CLASS_ET: return 0;
            case UCDN_BIDI_CLASS_AN: return 1;
            case UCDN_BIDI_CLASS_CS:
            case UCDN_BIDI_CLASS_NSM:
            case UCDN_BIDI_CLASS_BN:
            case UCDN_BIDI_CLASS_B:
            case UCDN_BIDI_CLASS_S:
            case UCDN_BIDI_CLASS_WS:
            case UCDN_BIDI_CLASS_ON:
                return cur_bidi;
            default:
                return 0;
        }
    };

    auto current_text_color = [&]() -> uint32_t {
        switch (text_render_mode) {
            case 1:
            case 5:
                return current_stroke_color;
            default:
                return current_fill_color;
        }
    };

    auto flush_remaining_actualtext = [&](std::vector<int>& text, float adv) {
        if (text.empty()) {
            return;
        }

        const float fs = st.font_size;
        const float rise = st.text_rise;
        const float h_scale = st.h_scale / 100.0f;
        float stm[6] = {
            fs * h_scale * st.tm[0], fs * h_scale * st.tm[1],
            fs * st.tm[2], fs * st.tm[3],
            rise * st.tm[2] + st.tm[4],
            rise * st.tm[3] + st.tm[5]
        };

        float m[6];
        m[0] = stm[0] * ctm[0] + stm[1] * ctm[2];
        m[1] = stm[0] * ctm[1] + stm[1] * ctm[3];
        m[2] = stm[2] * ctm[0] + stm[3] * ctm[2];
        m[3] = stm[2] * ctm[1] + stm[3] * ctm[3];
        m[4] = stm[4] * ctm[0] + stm[5] * ctm[2] + ctm[4];
        m[5] = stm[4] * ctm[1] + stm[5] * ctm[3] + ctm[5];

        const bool actualtext_clipped = clip_has_box && glyph_entirely_outside_box(
            m,
            adv,
            st.wmode,
            active_ascender,
            active_descender,
            current_clip_box);

        if (!actualtext_clipped) {
            for (int cp : text) {
                if (cp <= 0 || cp > 0x10FFFF) {
                    continue;
                }

                int bidi_class = ucdn_get_bidi_class(static_cast<uint32_t>(cp));
                active_bidi = guess_bidi_level(bidi_class, active_bidi);

                float adv = 0.0f;
                // calculate adv from font_width_map
                auto it = font_width_map.find(st.font_name);
                if (it != font_width_map.end()) {
                    auto w_it = it->second->find(cp);
                    if (w_it != it->second->end()) {
                        adv = w_it->second;
                    }
                }
                
                float adv_unscaled = adv / 1000.0f;
                if (st.font_size > 0.001f || st.font_size < -0.001f) {
                    adv_unscaled += st.char_spacing / st.font_size;
                    if (cp == 32) adv_unscaled += st.word_spacing / st.font_size;
                }

                // skip rendering if outside clip box
                if (actualtext_clipped) {
                    continue;
                }

                float m_copy[6] = {m[0], m[1], m[2], m[3], m[4], m[5]};
                extractor.add_char(
                    cp,
                    0.0f,
                    0.0f,
                    adv_unscaled,
                    m_copy,
                    st.font_name,
                    st.font_size,
                    current_text_color(),
                    active_is_bold, active_is_italic, active_is_serif, active_is_mono, st.wmode,
                    active_ascender, active_descender, active_bidi,
                    true);

                // apply scaling and character spacing for the NEXT character
                adv = (adv / 1000.0f) * st.font_size + st.char_spacing;
                if (cp == 32) adv += st.word_spacing;
                adv *= (st.h_scale / 100.0f);

                m[4] += adv * m[0];
                m[5] += adv * m[1];
            }
        }

        text.clear();
    };

    auto emit_text = [&](const std::vector<uint8_t>& bytes) {
        struct TextGlyphItem {
            int primary = 0;
            std::vector<int> unicode_seq;
            float adv = 0.0f;
            float m[6] = {1, 0, 0, 1, 0, 0};
            bool clipped = false;
            bool cid_fallback = false;
        };

        auto code_in_codespace = [](uint32_t code_value, int nbytes, const std::vector<WinCodeSpaceRange>& ranges) {
            for (const auto& r : ranges) {
                if (r.nbytes == nbytes && code_value >= r.low && code_value <= r.high) {
                    return true;
                }
            }
            return false;
        };

        auto sanitize_unicode_sequence = [](int code, std::vector<int>& seq) -> bool {
            auto is_valid_scalar = [](int cp) {
                return cp > 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
            };

            if (seq.empty()) {
                seq.push_back(code);
                return false;
            }


            std::vector<int> clean;
            clean.reserve(seq.size());
            for (int cp : seq) {
                if (is_valid_scalar(cp)) {
                    clean.push_back(cp);
                }
            }

            if (clean.empty()) {
                clean.push_back(code);
                seq.swap(clean);
                return false;
            }
            seq.swap(clean);
            return false;
        };

        auto emit_rune = [&](int rune, float adv, const float in_m[6], bool has_real_glyph, bool preserve_bidi = false) {
            auto dispatch_char = [&](int cp, float char_adv, bool is_primary, bool keep_current_bidi) {
                if (cp <= 0 || cp > 0x10FFFF) {
                    cp = 0xFFFD; 
                }
                
                if (!keep_current_bidi) {
                    int bidi_class = UCDN_BIDI_CLASS_ON;
                    if (cp > 0 && cp <= 0x10FFFF) {
                        bidi_class = ucdn_get_bidi_class(static_cast<uint32_t>(cp));
                    }
                    active_bidi = guess_bidi_level(bidi_class, active_bidi);
                }

                const uint32_t used_color = current_text_color();

                float m_copy[6] = {in_m[0], in_m[1], in_m[2], in_m[3], in_m[4], in_m[5]};
                extractor.add_char(
                    cp, 0.0f, 0.0f, char_adv, m_copy,
                    st.font_name, st.font_size, used_color,
                    active_is_bold, active_is_italic, active_is_serif, active_is_mono, st.wmode,
                    active_ascender, active_descender, active_bidi,
                    is_primary);
            };

                    dispatch_char(rune, adv, has_real_glyph, preserve_bidi);
        };

        const float h_scale = st.h_scale / 100.0f;
        std::vector<TextGlyphItem> glyphs;
        glyphs.reserve(bytes.size());

        size_t bi = 0;
        while (bi < bytes.size()) {
            const size_t code_start = bi;
            int code = static_cast<int>(bytes[bi]);
            int consumed = 1;
            int max_len = std::min<int>(active_code_bytes, static_cast<int>(bytes.size() - bi));
            if (max_len > 1) {
                bool matched = false;

                if (active_codespace_ranges && !active_codespace_ranges->empty()) {
                    for (int len = max_len; len >= 1; --len) {
                        uint32_t candidate = 0;
                        for (int k = 0; k < len; ++k) {
                            candidate = (candidate << 8) | static_cast<uint32_t>(bytes[bi + k]);
                        }
                        if (code_in_codespace(candidate, len, *active_codespace_ranges)) {
                            code = static_cast<int>(candidate);
                            consumed = len;
                            matched = true;
                            break;
                        }
                    }
                }

                if (!matched && (!active_codespace_ranges || active_codespace_ranges->empty())) {
                    uint32_t candidate = 0;
                    for (int k = 0; k < max_len; ++k) {
                        candidate = (candidate << 8) | static_cast<uint32_t>(bytes[bi + k]);
                    }
                    code = static_cast<int>(candidate);
                    consumed = max_len;
                    matched = true;
                }

                if (!matched) {
                    code = static_cast<int>(bytes[bi]);
                    consumed = 1;
                }
            }
            bi += static_cast<size_t>(consumed);

            float glyph_adv = active_default_advance;
            float v_x = glyph_adv / 2.0f;
            float v_y = 0.88f;

            if (st.wmode == 1) {
                glyph_adv = -1.0f; // Default vertical advance (DW2 w2_y default is -1000)
                if (active_w2_map) {
                    auto w2_it = active_w2_map->find(code);
                    if (w2_it != active_w2_map->end()) {
                        glyph_adv = w2_it->second.w2_y;
                        v_x = w2_it->second.v_x;
                        v_y = w2_it->second.v_y;
                    } else {
                        auto def_it = active_w2_map->find(-1);
                        if (def_it != active_w2_map->end()) {
                            glyph_adv = def_it->second.w2_y;
                            v_x = def_it->second.v_x;
                            v_y = def_it->second.v_y;
                        }
                    }
                }
            } else {
                if (active_width_map) {
                    auto wit = active_width_map->find(code);
                    if (wit != active_width_map->end()) {
                        glyph_adv = wit->second;
                    } else {
                        auto def_it = active_width_map->find(-1);
                        if (def_it != active_width_map->end()) {
                            glyph_adv = def_it->second;
                        } else if (code == ' ') {
                            glyph_adv = 0.33f;
                        }
                    }
                } else if (code == ' ') {
                    glyph_adv = 0.33f;
                }
            }

            const float fs = st.font_size;
            const float rise = st.text_rise;
            
            float stm[6];
            if (st.wmode == 1) {
                float ox = -v_x * fs;
                float oy = -v_y * fs;
                stm[0] = fs * st.tm[0];
                stm[1] = fs * st.tm[1];
                stm[2] = fs * st.tm[2];
                stm[3] = fs * st.tm[3];
                stm[4] = (rise + oy) * st.tm[2] + ox * st.tm[0] + st.tm[4];
                stm[5] = (rise + oy) * st.tm[3] + ox * st.tm[1] + st.tm[5];
            } else {
                stm[0] = fs * h_scale * st.tm[0];
                stm[1] = fs * h_scale * st.tm[1];
                stm[2] = fs * st.tm[2];
                stm[3] = fs * st.tm[3];
                stm[4] = rise * st.tm[2] + st.tm[4];
                stm[5] = rise * st.tm[3] + st.tm[5];
            }

            float m[6];
            m[0] = stm[0] * ctm[0] + stm[1] * ctm[2];
            m[1] = stm[0] * ctm[1] + stm[1] * ctm[3];
            m[2] = stm[2] * ctm[0] + stm[3] * ctm[2];
            m[3] = stm[2] * ctm[1] + stm[3] * ctm[3];
            m[4] = stm[4] * ctm[0] + stm[5] * ctm[2] + ctm[4];
            m[5] = stm[4] * ctm[1] + stm[5] * ctm[3] + ctm[5];

            bool glyph_clipped = false;
            if (clip_has_box) {
                glyph_clipped = glyph_entirely_outside_box(
                    m, glyph_adv, st.wmode, active_ascender, active_descender, current_clip_box);
            }            
            std::vector<int> unicode_seq;
            if (active_font_map) {
                auto it = active_font_map->find(code);
                if (it != active_font_map->end()) {
                    unicode_seq = it->second;


                    for (int& ucs : unicode_seq) {
                        if (ucs >= 8 && ucs <= 13) {
                            ucs = ' ';
                        }
                    }

                    bool has_invalid = false;
                    for (int& ucs : unicode_seq) {
                        if (ucs >= 128 && ucs <= 159) {
                            static const unsigned short win_ansi_to_unicode[32] = {
                                0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
                                0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178
                            };
                            unsigned short mapped = win_ansi_to_unicode[ucs - 128];
                            if (mapped != 0) ucs = mapped;
                        }

                        if (ucs >= 0 && ucs < 32 && ucs != 9 && ucs != 10 && ucs != 13) {
                            has_invalid = true;
                            break;
                        }
                    }
                    if (has_invalid) {
                        unicode_seq.clear();
                    }
                }
            }

            bool cid_fallback = false;

            if (unicode_seq.empty()) {
                unicode_seq.push_back(code);
                cid_fallback = false; 
            } else {
                cid_fallback = sanitize_unicode_sequence(code, unicode_seq);
            }

            const bool is_word_space = ((consumed == 1 && code == 0x20) || (consumed == 2 && code == 0x0020)) && st.wmode == 0;
            float tx = 0.0f;
            float ty = 0.0f;
            float adv_unscaled = glyph_adv;

            if (st.wmode == 0) {
                tx = (glyph_adv * fs + st.char_spacing) * h_scale;
                if (is_word_space) {
                    tx += st.word_spacing * h_scale;
                }
                st.tm[4] += tx * st.tm[0];
                st.tm[5] += tx * st.tm[1];

                if (fs > 0.001f || fs < -0.001f) {
                    adv_unscaled = glyph_adv + (st.char_spacing / fs);
                    if (is_word_space) {
                        adv_unscaled += (st.word_spacing / fs);
                    }
                }
            } else {
                ty = (glyph_adv * fs + st.char_spacing);
                if (is_word_space) {
                    ty += st.word_spacing;
                }
                st.tm[4] += ty * st.tm[2];
                st.tm[5] += ty * st.tm[3];

                if (fs > 0.001f || fs < -0.001f) {
                    adv_unscaled = glyph_adv + (st.char_spacing / fs);
                    if (is_word_space) {
                        adv_unscaled += (st.word_spacing / fs);
                    }
                }
            }

            TextGlyphItem item;
            item.primary = unicode_seq.front();
            item.unicode_seq = std::move(unicode_seq);
            item.adv = adv_unscaled;
            item.m[0] = m[0]; item.m[1] = m[1]; item.m[2] = m[2];
            item.m[3] = m[3]; item.m[4] = m[4]; item.m[5] = m[5];
            item.clipped = glyph_clipped;
            item.cid_fallback = cid_fallback;
            glyphs.push_back(std::move(item));
        }

        ActualTextState* at = active_actualtext();
        
        if (at == nullptr || at->text.empty()) {
            for (const auto& item : glyphs) {
                if (item.clipped) continue;
                emit_rune(item.primary, item.adv, item.m, true, item.cid_fallback);
                for (size_t si = 1; si < item.unicode_seq.size(); ++si) {
                    emit_rune(item.unicode_seq[si], 0.0f, item.m, false);
                }
            }
            return;
        }

        size_t start = 0;
        size_t end = glyphs.size();
        size_t actual_len = at->text.size();
        size_t actual_start = 0;
        size_t actual_end = actual_len;

        for (start = 0; start < glyphs.size(); ++start) {
            if (actual_start >= actual_len || glyphs[start].primary != at->text[actual_start]) {
                break;
            }
            actual_start++;
        }

        if (start != 0) {
            for (size_t i = 0; i < start; ++i) {
                if (!glyphs[i].clipped) emit_rune(glyphs[i].primary, glyphs[i].adv, glyphs[i].m, true);
            }
        }

        if (start == glyphs.size()) {
            at->text.erase(at->text.begin(), at->text.begin() + actual_start);
            return;
        }

        for (end = glyphs.size(); end > start; --end) {
            if (actual_end <= actual_start || glyphs[end - 1].primary != at->text[actual_end - 1]) {
                break;
            }
            actual_end--;
        }

        for (size_t i = start; i < end; ++i) {
            if (glyphs[i].clipped) continue;

            int rune = -1;
            if (actual_start < actual_end) {
                rune = at->text[actual_start++];
            }

            if (rune != -1) {
                emit_rune(rune, glyphs[i].adv, glyphs[i].m, true);
            } else {
                emit_rune(glyphs[i].primary, glyphs[i].adv, glyphs[i].m, false);
            }
        }

        if (end == glyphs.size()) {
            at->text.erase(at->text.begin(), at->text.begin() + actual_start);
            return;
        }

        if (actual_start < actual_end) {
            float base_m[6] = {1, 0, 0, 1, 0, 0};
            if (end > 0 && end <= glyphs.size()) {
                copy_matrix(base_m, glyphs[end - 1].m);
                if (st.wmode == 0) {
                    base_m[4] += glyphs[end - 1].adv * base_m[0];
                    base_m[5] += glyphs[end - 1].adv * base_m[1];
                } else {
                    base_m[4] += glyphs[end - 1].adv * base_m[2];
                    base_m[5] += glyphs[end - 1].adv * base_m[3];
                }
            }
            while (actual_start < actual_end) {
                emit_rune(at->text[actual_start++], 0.0f, base_m, false);
            }
        }

        if (end != glyphs.size()) {
            for (size_t i = end; i < glyphs.size(); ++i) {
                if (!glyphs[i].clipped) emit_rune(glyphs[i].primary, glyphs[i].adv, glyphs[i].m, true);
            }
        }

        at->text.clear();
    };


    for (size_t i = 0; i < stream.size();) {
        skip_ws_and_comments(stream, i);
        if (i >= stream.size()) {
            break;
        }

        PdfToken tok;
        if (parse_operand(stream, i, tok)) {
            operands.push_back(std::move(tok));
            continue;
        }

        if (is_delimiter(stream[i])) {
            ++i;
            continue;
        }

        std::string op = parse_operator(stream, i);
        if (op.empty()) {
            ++i;
            continue;
        }

        if (op == "BI") {
            skip_inline_image(stream, i);
        } else if (op == "BT") {
            in_text_object = true;
            set_identity(st.tm);
            set_identity(st.tlm);
            active_bidi = 0;
            extractor.hint_new_text_obj();
        } else if (op == "q") {
            GraphicsStateSnapshot snap;
            snap.ctm = {ctm[0], ctm[1], ctm[2], ctm[3], ctm[4], ctm[5]};
            snap.has_clip = clip_has_box;
            snap.clip = current_clip_box;
            snap.text_state = st;
            snap.font_map = active_font_map;
            snap.width_map = active_width_map;
            snap.w2_map = active_w2_map;
            snap.codespace_ranges = active_codespace_ranges;
            snap.code_bytes = active_code_bytes;
            snap.default_advance = active_default_advance;
            snap.is_italic = active_is_italic;
            snap.is_bold = active_is_bold;
            snap.ascender = active_ascender;
            snap.descender = active_descender;
            snap.bidi = active_bidi;
            snap.fill_color = current_fill_color;
            snap.stroke_color = current_stroke_color;
            snap.fill_space = current_fill_space;
            snap.stroke_space = current_stroke_space;
            snap.render_mode = text_render_mode;
            gstate_stack.push_back(snap);
        } else if (op == "Q") {
            if (!gstate_stack.empty()) {
                GraphicsStateSnapshot top = gstate_stack.back();
                gstate_stack.pop_back();
                for (int k = 0; k < 6; ++k) {
                    ctm[k] = top.ctm[k];
                }
                clip_has_box = top.has_clip;
                current_clip_box = top.clip;
                st = std::move(top.text_state);
                active_font_map = top.font_map;
                active_width_map = top.width_map;
                active_w2_map = top.w2_map;
                active_codespace_ranges = top.codespace_ranges;
                active_code_bytes = top.code_bytes;
                active_default_advance = top.default_advance;
                active_is_italic = top.is_italic;
                active_is_bold = top.is_bold;
                active_ascender = top.ascender;
                active_descender = top.descender;
                active_bidi = top.bidi;
                current_fill_color = top.fill_color;
                current_stroke_color = top.stroke_color;
                current_fill_space = top.fill_space;
                current_stroke_space = top.stroke_space;
                text_render_mode = top.render_mode;
            }
        } else if (op == "cm") {
            if (operands.size() >= 6) {
                bool ok = true;
                float m2[6];
                for (int k = 0; k < 6; ++k) {
                    const PdfToken& t = operands[operands.size() - 6 + k];
                    if (t.type != PdfToken::Type::Number) {
                        ok = false;
                        break;
                    }
                    m2[k] = static_cast<float>(t.number);
                }
                if (ok) {
                    float out[6];
                    matrix_multiply(m2, ctm, out);
                    for (int k = 0; k < 6; ++k) {
                        ctm[k] = out[k];
                    }
                }
            }
        } else if (op == "m") {
            if (operands.size() >= 2) {
                const PdfToken& x = operands[operands.size() - 2];
                const PdfToken& y = operands[operands.size() - 1];
                if (x.type == PdfToken::Type::Number && y.type == PdfToken::Type::Number) {
                    Vec2 p = apply_matrix_to_point(ctm, static_cast<float>(x.number), static_cast<float>(y.number));
                    add_path_point(p);
                    path_current = p;
                    path_subpath_start = p;
                    path_has_current = true;
                }
            }
        } else if (op == "l") {
            if (operands.size() >= 2) {
                const PdfToken& x = operands[operands.size() - 2];
                const PdfToken& y = operands[operands.size() - 1];
                if (x.type == PdfToken::Type::Number && y.type == PdfToken::Type::Number) {
                    Vec2 p = apply_matrix_to_point(ctm, static_cast<float>(x.number), static_cast<float>(y.number));
                    add_path_point(p);
                    path_current = p;
                    path_has_current = true;
                }
            }
        } else if (op == "c") {
            if (operands.size() >= 6) {
                const PdfToken& x1 = operands[operands.size() - 6];
                const PdfToken& y1 = operands[operands.size() - 5];
                const PdfToken& x2 = operands[operands.size() - 4];
                const PdfToken& y2 = operands[operands.size() - 3];
                const PdfToken& x3 = operands[operands.size() - 2];
                const PdfToken& y3 = operands[operands.size() - 1];
                if (x1.type == PdfToken::Type::Number && y1.type == PdfToken::Type::Number &&
                    x2.type == PdfToken::Type::Number && y2.type == PdfToken::Type::Number &&
                    x3.type == PdfToken::Type::Number && y3.type == PdfToken::Type::Number) {
                    add_path_point(apply_matrix_to_point(ctm, static_cast<float>(x1.number), static_cast<float>(y1.number)));
                    add_path_point(apply_matrix_to_point(ctm, static_cast<float>(x2.number), static_cast<float>(y2.number)));
                    Vec2 p3 = apply_matrix_to_point(ctm, static_cast<float>(x3.number), static_cast<float>(y3.number));
                    add_path_point(p3);
                    path_current = p3;
                    path_has_current = true;
                }
            }
        } else if (op == "v") {
            if (operands.size() >= 4) {
                const PdfToken& x2 = operands[operands.size() - 4];
                const PdfToken& y2 = operands[operands.size() - 3];
                const PdfToken& x3 = operands[operands.size() - 2];
                const PdfToken& y3 = operands[operands.size() - 1];
                if (x2.type == PdfToken::Type::Number && y2.type == PdfToken::Type::Number &&
                    x3.type == PdfToken::Type::Number && y3.type == PdfToken::Type::Number) {
                    add_path_point(apply_matrix_to_point(ctm, static_cast<float>(x2.number), static_cast<float>(y2.number)));
                    Vec2 p3 = apply_matrix_to_point(ctm, static_cast<float>(x3.number), static_cast<float>(y3.number));
                    add_path_point(p3);
                    path_current = p3;
                    path_has_current = true;
                }
            }
        } else if (op == "y") {
            if (operands.size() >= 4) {
                const PdfToken& x1 = operands[operands.size() - 4];
                const PdfToken& y1 = operands[operands.size() - 3];
                const PdfToken& x3 = operands[operands.size() - 2];
                const PdfToken& y3 = operands[operands.size() - 1];
                if (x1.type == PdfToken::Type::Number && y1.type == PdfToken::Type::Number &&
                    x3.type == PdfToken::Type::Number && y3.type == PdfToken::Type::Number) {
                    add_path_point(apply_matrix_to_point(ctm, static_cast<float>(x1.number), static_cast<float>(y1.number)));
                    Vec2 p3 = apply_matrix_to_point(ctm, static_cast<float>(x3.number), static_cast<float>(y3.number));
                    add_path_point(p3);
                    path_current = p3;
                    path_has_current = true;
                }
            }
        } else if (op == "h") {
            if (path_has_current) {
                add_path_point(path_subpath_start);
                path_current = path_subpath_start;
            }
        } else if (op == "re") {
            if (operands.size() >= 4) {
                const PdfToken& x = operands[operands.size() - 4];
                const PdfToken& y = operands[operands.size() - 3];
                const PdfToken& w = operands[operands.size() - 2];
                const PdfToken& h = operands[operands.size() - 1];
                if (x.type == PdfToken::Type::Number && y.type == PdfToken::Type::Number &&
                    w.type == PdfToken::Type::Number && h.type == PdfToken::Type::Number) {
                    const float xf = static_cast<float>(x.number);
                    const float yf = static_cast<float>(y.number);
                    const float wf = static_cast<float>(w.number);
                    const float hf = static_cast<float>(h.number);

                    Vec2 p0 = apply_matrix_to_point(ctm, xf, yf);
                    Vec2 p1 = apply_matrix_to_point(ctm, xf + wf, yf);
                    Vec2 p2 = apply_matrix_to_point(ctm, xf + wf, yf + hf);
                    Vec2 p3 = apply_matrix_to_point(ctm, xf, yf + hf);
                    add_path_point(p0);
                    add_path_point(p1);
                    add_path_point(p2);
                    add_path_point(p3);
                    path_current = p0;
                    path_subpath_start = p0;
                    path_has_current = true;
                }
            }
        } else if (op == "W" || op == "W*") {
            pending_clip = true;
        } else if (op == "S" || op == "s" || op == "f" || op == "F" || op == "f*" ||
                   op == "B" || op == "B*" || op == "b" || op == "b*" || op == "n") {
            consume_current_path();
        } else if (op == "Do") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Name) {
                const std::string& xobj_name = operands.back().name;
                
                // 1. Try to find it in image_xobject_map
                bool handled_as_image = false;
                if (image_xobject_map) {
                    auto iit = image_xobject_map->find(xobj_name);
                    if (iit != image_xobject_map->end()) {
                        handled_as_image = true;
                        
                        // Extract image bounds from CTM
                        // The image is mapped to the unit square [0,0] to [1,1] before CTM
                        Vec2 p0 = apply_matrix_to_point(ctm, 0, 0);
                        Vec2 p1 = apply_matrix_to_point(ctm, 1, 0);
                        Vec2 p2 = apply_matrix_to_point(ctm, 1, 1);
                        Vec2 p3 = apply_matrix_to_point(ctm, 0, 1);
                        
                        float min_x = std::min({p0.x, p1.x, p2.x, p3.x});
                        float max_x = std::max({p0.x, p1.x, p2.x, p3.x});
                        float min_y = std::min({p0.y, p1.y, p2.y, p3.y});
                        float max_y = std::max({p0.y, p1.y, p2.y, p3.y});
                        
                        Rect bbox = {min_x, min_y, max_x, max_y};
                        
                        extractor.add_image(bbox, iit->second);
                    }
                }
                
                // 2. Try to find it in form_xobject_map
                if (!handled_as_image && form_xobject_map) {
                    auto fit = form_xobject_map->find(xobj_name);
                    if (fit != form_xobject_map->end() && fit->second.stream_ptr && !fit->second.stream_ptr->empty()) {
                        float form_ctm[6];
                        matrix_multiply(fit->second.matrix.data(), ctm, form_ctm);
                        WinPdfInterpreter::run(*(fit->second.stream_ptr),
                                               extractor,
                                               fit->second.font_unicode_map,
                                               fit->second.font_width_map,
                                               fit->second.font_code_bytes_map,
                                               fit->second.font_codespace_map,
                                               fit->second.font_matrix_map,
                                               fit->second.font_vertical_metrics_map,
                                               fit->second.font_w2_map,
                                               fit->second.color_space_map,
                                               fit->second.children,
                                               image_xobject_map,
                                               form_ctm,
                                               recursion_depth + 1,
                                               page_mediabox,
                                               clip_has_box ? &current_clip_box : nullptr,
                                               &current_fill_color,
                                               &current_stroke_color,
                                               &current_fill_space,
                                               &current_stroke_space,
                                               &text_render_mode);
                    }
                }
            }
        } else if (op == "BDC") {
            ActualTextState* active = active_actualtext();
            if (active != nullptr && !active->text.empty()) {
                flush_remaining_actualtext(active->text, 0.0f);
            }

            ActualTextState state;
            state.text = decode_actualtext_from_operands(operands);
            actual_text_stack.push_back(std::move(state));
        } else if (op == "BMC") {
            actual_text_stack.push_back(ActualTextState{});
        } else if (op == "DP") {
            ActualTextState* active = active_actualtext();
            if (active != nullptr && !active->text.empty()) {
                flush_remaining_actualtext(active->text, 0.0f);
            }

            std::vector<int> point_actualtext = decode_actualtext_from_operands(operands);
            if (!point_actualtext.empty()) {
                flush_remaining_actualtext(point_actualtext, 0.0f);
            }
        } else if (op == "MP") {
            // Marked-content points have no text payload for this extractor path.
        } else if (op == "EMC") {
            if (!actual_text_stack.empty()) {
                ActualTextState state = std::move(actual_text_stack.back());
                actual_text_stack.pop_back();

                if (!state.text.empty()) {
                    flush_remaining_actualtext(state.text, 0.0f);
                }
            }
        } else if (op == "ET") {
            in_text_object = false;
        } else if (op == "Tf") {
            if (operands.size() >= 2) {
                const PdfToken& font = operands[operands.size() - 2];
                const PdfToken& size = operands[operands.size() - 1];
                if (font.type == PdfToken::Type::Name && size.type == PdfToken::Type::Number) {
                    st.font_name = font.name;
                    st.font_size = static_cast<float>(size.number);
                    active_font_map  = nullptr;
                    active_width_map = nullptr;
                    active_codespace_ranges = nullptr;
                    active_code_bytes = 1;
                    active_default_advance = 0.55f;
                    active_is_italic = false;
                    active_is_bold = false;
                    active_is_serif = true;
                    active_is_mono = false;
                    active_ascender = 0.8f;
                    active_descender = -0.2f;
                    active_w2_map = nullptr;
                    st.wmode = 0;

                    std::string real_base_font_name = "";

                    const std::string lower_font = to_lower_ascii(normalize_pdf_font_name(st.font_name));
                    if (lower_font.find("bold") != std::string::npos ||
                        lower_font.find("black") != std::string::npos ||
                        lower_font.find("heavy") != std::string::npos ||
                        lower_font.find("demi") != std::string::npos) {
                        active_is_bold = true;
                    }

                    auto mit = font_matrix_map.find(st.font_name);
                    if (mit != font_matrix_map.end()) {
                        active_is_italic = std::fabs(mit->second[2]) > 0.0001f;
                    }

                    auto cbit = font_code_bytes_map.find(st.font_name);
                    if (cbit != font_code_bytes_map.end() && cbit->second > 0) {
                        active_code_bytes = cbit->second;
                    }

                    auto csit = font_codespace_map.find(st.font_name);
                    if (csit != font_codespace_map.end() && !csit->second->empty()) {
                        active_codespace_ranges = csit->second.get();
                        for (const auto& r : *active_codespace_ranges) {
                            if (r.nbytes > active_code_bytes) {
                                active_code_bytes = r.nbytes;
                            }
                        }
                    }

                    auto fit = font_unicode_map.find(st.font_name);
                    if (fit != font_unicode_map.end()) {
                        active_font_map = fit->second.get();
                    }

                    auto vit = font_vertical_metrics_map.find(st.font_name);
                    if (vit != font_vertical_metrics_map.end()) {
                        active_ascender = vit->second.ascender;
                        active_descender = vit->second.descender;
                        active_is_bold = vit->second.is_bold;
                        active_is_italic = vit->second.is_italic;
                        active_is_serif = vit->second.is_serif;
                        active_is_mono = vit->second.is_mono;
                        st.wmode = vit->second.wmode;
                        if (!vit->second.base_font.empty()) {
                            real_base_font_name = vit->second.base_font;
                        }
                    }

                    auto wit = font_width_map.find(st.font_name);
                    if (wit != font_width_map.end()) {
                        active_width_map = wit->second.get();
                        auto def_it = wit->second->find(-1);
                        if (def_it != wit->second->end()) {
                            active_default_advance = def_it->second;
                        } else {
                            active_default_advance = 0.55f;
                        }

                        auto italic_it = wit->second->find(-2);
                        if (italic_it != wit->second->end() && italic_it->second > 0.0f) {
                            active_is_italic = true;
                        }

                        auto bold_it = wit->second->find(-3);
                        if (bold_it != wit->second->end() && bold_it->second > 0.0f) {
                            active_is_bold = true;
                        }
                    }

                    auto w2it = font_w2_map.find(st.font_name);
                    if (w2it != font_w2_map.end()) {
                        active_w2_map = w2it->second.get();
                    }

                    if (!real_base_font_name.empty()) {
                        st.font_name = real_base_font_name;
                    }

                }
            }
        } else if (op == "Tc") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                st.char_spacing = static_cast<float>(operands.back().number);
            }
        } else if (op == "Tw") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                st.word_spacing = static_cast<float>(operands.back().number);
            }
        } else if (op == "Tz") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                st.h_scale = static_cast<float>(operands.back().number);
            }
        } else if (op == "TL") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                st.leading = static_cast<float>(operands.back().number);
            }
        } else if (op == "Ts") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                st.text_rise = static_cast<float>(operands.back().number);
            }
        } else if (op == "Tr") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                int mode = static_cast<int>(std::lround(operands.back().number));
                if (mode < 0) mode = 0;
                if (mode > 7) mode = 7;
                text_render_mode = mode;
            }
        } else if (op == "cs") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Name) {
                current_fill_space = resolve_named_color_space(operands.back().name, current_fill_space);
            }
        } else if (op == "CS") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Name) {
                current_stroke_space = resolve_named_color_space(operands.back().name, current_stroke_space);
            }
        } else if (op == "sc" || op == "scn") {
            apply_color_operands(current_fill_space, operands, current_fill_color);
        } else if (op == "SC" || op == "SCN") {
            apply_color_operands(current_stroke_space, operands, current_stroke_color);
        } else if (op == "g") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                current_fill_color = rgb_from_gray(static_cast<float>(operands.back().number));
                current_fill_space = make_colorspace_def(WinColorSpaceKind::DeviceGray, 1);
            }
        } else if (op == "G") {
            if (!operands.empty() && operands.back().type == PdfToken::Type::Number) {
                current_stroke_color = rgb_from_gray(static_cast<float>(operands.back().number));
                current_stroke_space = make_colorspace_def(WinColorSpaceKind::DeviceGray, 1);
            }
        } else if (op == "rg") {
            if (operands.size() >= 3) {
                const PdfToken& r = operands[operands.size() - 3];
                const PdfToken& g = operands[operands.size() - 2];
                const PdfToken& b = operands[operands.size() - 1];
                if (r.type == PdfToken::Type::Number && g.type == PdfToken::Type::Number && b.type == PdfToken::Type::Number) {
                    current_fill_color = (to_u8(static_cast<float>(r.number)) << 16) |
                                         (to_u8(static_cast<float>(g.number)) << 8) |
                                         to_u8(static_cast<float>(b.number));
                    current_fill_space = make_colorspace_def(WinColorSpaceKind::DeviceRGB, 3);
                }
            }
        } else if (op == "RG") {
            if (operands.size() >= 3) {
                const PdfToken& r = operands[operands.size() - 3];
                const PdfToken& g = operands[operands.size() - 2];
                const PdfToken& b = operands[operands.size() - 1];
                if (r.type == PdfToken::Type::Number && g.type == PdfToken::Type::Number && b.type == PdfToken::Type::Number) {
                    current_stroke_color = (to_u8(static_cast<float>(r.number)) << 16) |
                                           (to_u8(static_cast<float>(g.number)) << 8) |
                                           to_u8(static_cast<float>(b.number));
                    current_stroke_space = make_colorspace_def(WinColorSpaceKind::DeviceRGB, 3);
                }
            }
        } else if (op == "k") {
            if (operands.size() >= 4) {
                const PdfToken& c = operands[operands.size() - 4];
                const PdfToken& m = operands[operands.size() - 3];
                const PdfToken& y = operands[operands.size() - 2];
                const PdfToken& k = operands[operands.size() - 1];
                if (c.type == PdfToken::Type::Number && m.type == PdfToken::Type::Number && y.type == PdfToken::Type::Number && k.type == PdfToken::Type::Number) {
                    current_fill_color = rgb_from_cmyk(
                        static_cast<float>(c.number),
                        static_cast<float>(m.number),
                        static_cast<float>(y.number),
                        static_cast<float>(k.number));
                    current_fill_space = make_colorspace_def(WinColorSpaceKind::DeviceCMYK, 4);
                }
            }
        } else if (op == "K") {
            if (operands.size() >= 4) {
                const PdfToken& c = operands[operands.size() - 4];
                const PdfToken& m = operands[operands.size() - 3];
                const PdfToken& y = operands[operands.size() - 2];
                const PdfToken& k = operands[operands.size() - 1];
                if (c.type == PdfToken::Type::Number && m.type == PdfToken::Type::Number && y.type == PdfToken::Type::Number && k.type == PdfToken::Type::Number) {
                    current_stroke_color = rgb_from_cmyk(
                        static_cast<float>(c.number),
                        static_cast<float>(m.number),
                        static_cast<float>(y.number),
                        static_cast<float>(k.number));
                    current_stroke_space = make_colorspace_def(WinColorSpaceKind::DeviceCMYK, 4);
                }
            }
        } else if (op == "Tm") {
            if (operands.size() >= 6) {
                bool ok = true;
                float m[6];
                for (int k = 0; k < 6; ++k) {
                    const PdfToken& t = operands[operands.size() - 6 + k];
                    if (t.type != PdfToken::Type::Number) {
                        ok = false;
                        break;
                    }
                    m[k] = static_cast<float>(t.number);
                }
                if (ok) {
                    for (int k = 0; k < 6; ++k) {
                        st.tm[k] = m[k];
                        st.tlm[k] = m[k];
                    }
                }
            }
        } else if (op == "Td") {
            if (operands.size() >= 2) {
                const PdfToken& tx = operands[operands.size() - 2];
                const PdfToken& ty = operands[operands.size() - 1];
                if (tx.type == PdfToken::Type::Number && ty.type == PdfToken::Type::Number) {
                    move_text_position(st, static_cast<float>(tx.number), static_cast<float>(ty.number));
                }
            }
        } else if (op == "TD") {
            if (operands.size() >= 2) {
                const PdfToken& tx = operands[operands.size() - 2];
                const PdfToken& ty = operands[operands.size() - 1];
                if (tx.type == PdfToken::Type::Number && ty.type == PdfToken::Type::Number) {
                    st.leading = -static_cast<float>(ty.number);
                    move_text_position(st, static_cast<float>(tx.number), static_cast<float>(ty.number));
                }
            }
        } else if (op == "T*") {
            move_text_position(st, 0.0f, -st.leading);
        } else if (op == "Tj") {
    if (in_text_object && !operands.empty() && operands.back().type == PdfToken::Type::String) { 
        emit_text(operands.back().bytes);   
    }
} else if (op == "TJ") {
    if (in_text_object && !operands.empty() && operands.back().type == PdfToken::Type::Array) {
        for (const PdfToken& item : operands.back().items) {
            if (item.type == PdfToken::Type::String) {
                emit_text(item.bytes);
            } else if (item.type == PdfToken::Type::Number) {
                float tadj = static_cast<float>(-item.number * st.font_size * 0.001);
                if (st.wmode == 0) {
                    const float tx = tadj * (st.h_scale / 100.0f);
                    st.tm[4] += tx * st.tm[0];
                    st.tm[5] += tx * st.tm[1];
                } else {
                    st.tm[4] += tadj * st.tm[2];
                    st.tm[5] += tadj * st.tm[3];
                }
            }
        }
    }
        } else if (op == "'") {
            move_text_position(st, 0.0f, -st.leading);
            if (in_text_object && !operands.empty() && operands.back().type == PdfToken::Type::String) {
                emit_text(operands.back().bytes);
            }
        } else if (op == "\"") {
            if (operands.size() >= 3) {
                const PdfToken& aw = operands[operands.size() - 3];
                const PdfToken& ac = operands[operands.size() - 2];
                const PdfToken& text = operands[operands.size() - 1];
                if (aw.type == PdfToken::Type::Number && ac.type == PdfToken::Type::Number) {
                    st.word_spacing = static_cast<float>(aw.number);
                    st.char_spacing = static_cast<float>(ac.number);
                    move_text_position(st, 0.0f, -st.leading);
                    if (in_text_object && text.type == PdfToken::Type::String) {
                        emit_text(text.bytes);
                    }
                }
            }
        }

        operands.clear();
    }

    while (!actual_text_stack.empty()) {
        ActualTextState state = std::move(actual_text_stack.back());
        actual_text_stack.pop_back();
        if (!state.text.empty()) {
            flush_remaining_actualtext(state.text, 0.0f);
        }
    }
}
