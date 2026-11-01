import importlib.util
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('pr_ai_check',ROOT/'.github/scripts/check_pr_ai.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)


class DisclosureTests(unittest.TestCase):
    def test_actual_names_and_explicit_unknown_are_accepted(self):
        for value in ('GPT-6 (Codex; exact variant unknown)','DeepSeek model version supplied by the tool','Unknown (tool did not disclose)','None (no AI used)'):
            self.assertTrue(module.check('AI models: '+value)[0])

    def test_template_placeholders_are_not_disclosures(self):
        template=(ROOT/'.github/pull_request_template.md').read_text(encoding='utf-8')
        self.assertFalse(module.check(template)[0])
        for text in ('','AI models:','AI models: TODO','AI models: [model]','<!-- AI models: model-name -->'):
            self.assertFalse(module.check(text)[0])

    def test_windows_line_endings_are_valid(self):
        self.assertTrue(module.check('Summary\r\n\r\nAI models: Unknown exact model/version\r\nAI scope: tests\r\n')[0])

    def test_ai_handoff_includes_conditional_fix_and_model_requirement(self):
        from manager.ai_context import build
        from manager.workspace import Workspace
        from manager.providers import attach
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            workspace=Workspace(tmp,ROOT/'mods');attach(workspace)
            text=build(workspace,'http://127.0.0.1:12345/',provider_id='shop',kind='bug')['text']
            self.assertIn('信息缺失时再问',text)
            self.assertIn('PR 必须有 AI models:',text)
            self.assertIn('Unknown',text)
            self.assertIn('Human verification',text)


if __name__=='__main__':unittest.main()
