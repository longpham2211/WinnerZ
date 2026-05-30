from pathlib import Path
import importlib.util


ROOT = Path(__file__).resolve().parents[1]
PDF_PATH = ROOT / "data" / "ieee-format.pdf"
APP_PATH = ROOT / "app-winnerz.py"


def load_app_module():
    spec = importlib.util.spec_from_file_location("app_winnerz", APP_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("Cannot load app-winnerz.py module spec")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    app_mod = load_app_module()
    app = app_mod.app

    with app.test_client() as client:
        with PDF_PATH.open("rb") as f:
            resp = client.post(
                "/api/upload",
                data={"file": (f, "ieee-format.pdf")},
                content_type="multipart/form-data",
            )
        assert resp.status_code == 200, resp.data
        up = resp.get_json()
        assert up and "doc_id" in up

        page_resp = client.get(f"/api/page/{up['doc_id']}/5")
        assert page_resp.status_code == 200, page_resp.data
        data = page_resp.get_json()
        assert data and isinstance(data.get("img"), str)
        assert len(data["img"]) > 1000
        assert data.get("w", 0) > 0 and data.get("h", 0) > 0

    print("PASS")
    print("doc_id", up["doc_id"])
    print("preview_size", data["w"], data["h"])
    print("group_count", len(data.get("groups", [])))


if __name__ == "__main__":
    main()
