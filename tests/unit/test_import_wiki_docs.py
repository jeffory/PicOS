"""Unit tests for tools/import_wiki_docs.py. Run: python3 -m pytest tests/unit/test_import_wiki_docs.py -v"""
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import import_wiki_docs as iw  # noqa: E402

INDEX = {"api-ui": "API-UI.md", "api-repl": "API-Repl.md", "home": "index.md",
         "native-app-development": "Native-Loading.md"}


def test_plain_link():
    assert iw.convert_links("See [[API UI]].", INDEX) == ("See [API UI](API-UI.md).", [])


def test_target_matching_ignores_case():
    assert iw.convert_links("[[API REPL]]", INDEX) == ("[API REPL](API-Repl.md)", [])


def test_text_and_page_form():
    assert iw.convert_links("[[the UI module|API UI]]", INDEX) == ("[the UI module](API-UI.md)", [])


def test_home_links_to_index():
    assert iw.convert_links("[[Home]]", INDEX) == ("[Home](index.md)", [])


def test_alias_for_a_page_that_never_existed():
    assert iw.convert_links("[[Native App Development]]", INDEX) == (
        "[Native App Development](Native-Loading.md)", [])


def test_unresolved_link_is_reported_and_left_alone():
    assert iw.convert_links("[[Nope]]", INDEX) == ("[[Nope]]", ["Nope"])


def test_code_is_left_alone():
    text = "```lua\nlocal s = [[API UI]]\n```\nuse `[[API UI]]` or [[API UI]]\n"
    out, bad = iw.convert_links(text, INDEX)
    assert out == "```lua\nlocal s = [[API UI]]\n```\nuse `[[API UI]]` or [API UI](API-UI.md)\n"
    assert bad == []


def test_split_title_takes_the_first_heading():
    assert iw.split_title("\n# API UI\n\nBody\n", "API-UI") == ("API UI", "Body\n")


def test_split_title_falls_back_to_the_file_name():
    assert iw.split_title("No heading\n", "Library-Download") == ("Library Download", "No heading\n")


def test_sidebar_groups_and_items():
    groups, bad = iw.sidebar("### Documentation\n* [[Home]]\n* [[API UI]]\n", INDEX)
    assert bad == []
    assert groups == [{"label": "Documentation", "items": [
        {"label": "Home", "page": "index.md"}, {"label": "API UI", "page": "API-UI.md"}]}]


def test_main_end_to_end(tmp_path):
    (tmp_path / "Home.md").write_text("# Welcome\n\nStart at [[API UI]].\n")
    (tmp_path / "API-UI.md").write_text("# API UI\n\nBack to [[Home]].\n")
    (tmp_path / "_Sidebar.md").write_text("### Docs\n* [[Home]]\n* [[API UI]]\n")
    (tmp_path / "_Footer.md").write_text("footer\n")
    assert iw.main([str(tmp_path)]) == 0
    assert sorted(p.name for p in tmp_path.iterdir()) == ["API-UI.md", "_sidebar.json", "index.md"]
    assert (tmp_path / "index.md").read_text() == '---\ntitle: "Welcome"\n---\n\nStart at [API UI](API-UI.md).\n'
    assert (tmp_path / "API-UI.md").read_text() == '---\ntitle: "API UI"\n---\n\nBack to [Home](index.md).\n'
    assert json.loads((tmp_path / "_sidebar.json").read_text()) == [{"label": "Docs", "items": [
        {"label": "Home", "page": "index.md"}, {"label": "API UI", "page": "API-UI.md"}]}]


def test_main_refuses_a_second_run(tmp_path):
    (tmp_path / "index.md").write_text('---\ntitle: "x"\n---\n\nbody\n')
    with pytest.raises(SystemExit, match="already"):
        iw.main([str(tmp_path)])


def test_main_writes_nothing_when_a_link_is_unresolved(tmp_path, capsys):
    (tmp_path / "Home.md").write_text("# Home\n\nSee [[Missing Page]].\n")
    assert iw.main([str(tmp_path)]) == 1
    assert "Missing Page" in capsys.readouterr().err
    assert sorted(p.name for p in tmp_path.iterdir()) == ["Home.md"]
