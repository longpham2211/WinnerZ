from typing import Any, Dict, Mapping, Optional, Sequence, Tuple

RectTuple = Tuple[float, float, float, float]


def page_count(pdf_path: str) -> int: ...


def page_rect(pdf_path: str, page_index: int = 0) -> RectTuple: ...


def extract_dict_json(pdf_path: str, page_index: int = 0, sort: bool = False) -> str: ...


def extract_rawdict_json(pdf_path: str, page_index: int = 0, sort: bool = False) -> str: ...


def extract_blocks_json(pdf_path: str, page_index: int = 0, sort: bool = False) -> str: ...


def extract_text(pdf_path: str, page_index: int = 0) -> str: ...


def extract_drawings_json(pdf_path: str, page_index: int = 0) -> str: ...


def redact_text_rects(
    input_pdf: str,
    output_pdf: str,
    page_index: int,
    rects: Sequence[Sequence[float]],
    min_overlap_ratio: float = 0.0,
) -> Mapping[str, int]: ...


def render_page_to_bytes(
    pdf_path: str,
    page_index: int = 0,
    scale: float = 1.0,
    clip: Optional[RectTuple] = None,
) -> Dict[str, Any]: ...
