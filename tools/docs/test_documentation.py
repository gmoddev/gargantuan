import importlib.util
import pathlib
import tempfile
import unittest
from unittest import mock

Spec = importlib.util.spec_from_file_location("Documentation", pathlib.Path(__file__).with_name("CheckDocumentation.py"))
Documentation = importlib.util.module_from_spec(Spec)
Spec.loader.exec_module(Documentation)


class DocumentationTests(unittest.TestCase):
    def test_source_hash_is_independent_of_checkout_newlines(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Folder = pathlib.Path(Temporary)
            (Folder / "lf.luau").write_bytes(b'local Value = 1\nreturn Value\n')
            (Folder / "crlf.luau").write_bytes(b'local Value = 1\r\nreturn Value\r\n')
            self.assertEqual(Documentation.SourceHash(Folder / "lf.luau"), Documentation.SourceHash(Folder / "crlf.luau"))

    def test_declarations_preserve_strings_and_exclude_commented_apis(self):
        Source = '''{Name = "a,b -- value", Methods = {Active = {Returns = t.Optional(t.Instance("Part"))}}, -- Removed = {},
                    Enabled = true}'''
        Fields = Documentation.Fields(Documentation.RemoveComments(Source))
        self.assertEqual(Documentation.Value(Fields["Name"]), "a,b -- value")
        self.assertEqual(list(Documentation.Fields(Fields["Methods"])), ["Active"])
        self.assertNotIn("Removed", Fields)

    def test_duplicate_and_unsupported_declarations_fail(self):
        with self.assertRaises(ValueError):
            Documentation.Fields('{Name = "one", Name = "two"}')
        with self.assertRaises(ValueError):
            Documentation.Fields('{[Expression()] = true}')

    def test_current_accepted_decisions_remain_normative(self):
        self.assertEqual(Documentation.Classify("docs/adr/D03.md", "status: accepted")[0], "Current")
        self.assertEqual(Documentation.Classify("docs/adr/D02.md", "status: historical-proposal")[0], "Historical")

    def test_directory_index_and_route_relative_links(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Content = pathlib.Path(Temporary)
            (Content / "api").mkdir()
            (Content / "api/index.mdx").write_text("# API", encoding="utf8")
            Source = Content / "index.mdx"
            with mock.patch.object(Documentation, "Content", Content):
                self.assertEqual(Documentation.Resolve(Source, "./api/"), (Content / "api/index.mdx").resolve())

    def test_route_relative_link_normalizes_virtual_directory(self):
        # The page's route directory does not physically exist. This catches
        # the POSIX traversal failure masked by Windows' path normalization.
        with tempfile.TemporaryDirectory() as Temporary:
            Content = pathlib.Path(Temporary)
            (Content / "guides").mkdir()
            Target = Content / "guides/first.mdx"
            Target.write_text("# First", encoding="utf8")
            Source = Content / "guides/other.mdx"
            with mock.patch.object(Documentation, "Content", Content):
                self.assertEqual(Documentation.Resolve(Source, "../first/"), Target.resolve())

    def test_missing_links_anchors_and_code_examples_are_reported(self):
        with tempfile.TemporaryDirectory() as Temporary:
            Folder = pathlib.Path(Temporary)
            Source = Folder / "a.md"
            (Folder / "b.md").write_text("# Valid heading\n", encoding="utf8")
            Source.write_text("[ok](b.md#valid-heading)\n[bad](b.md#missing)\n[absent](no.md)\n```md\n[code](no.md)\n```\n", encoding="utf8")
            with mock.patch.object(Documentation, "Root", Folder):
                self.assertEqual([Item["Reason"] for Item in Documentation.Links(Source)], ["Missing heading anchor", "Missing target"])


if __name__ == "__main__":
    unittest.main()
