"""Synthetic activation safety checks; never contacts SSH or OpenAI."""
import importlib.util
from pathlib import Path
import pytest

MODULE = Path(__file__).resolve().parents[1] / "deploy" / "activate_analysis.py"
spec = importlib.util.spec_from_file_location("activate_analysis", MODULE)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def config(key='synthetic-key'):
    return dict(zip(helper.NAMES, [key, 'true', helper.MODEL, '1']))


def test_merge_preserves_unrelated_fields_and_quotes_key():
    original = '# private settings\nRICE_DATA_DIR=/var/lib/rice-cloud\nRICE_ANALYSIS_ENABLED=false\nOTHER="leave this alone"'
    value = 'synthetic \\ " $ % key'
    result = helper.merge_environment(original, config(value))
    assert '# private settings\nRICE_DATA_DIR=/var/lib/rice-cloud\n' in result
    assert 'OTHER="leave this alone"\n' in result
    assert result.count('RICE_ANALYSIS_ENABLED=') == 1
    assert helper.assignments(result)['RICE_OPENAI_API_KEY'] == value


@pytest.mark.parametrize('field,value', [('RICE_OPENAI_API_KEY', ''), ('RICE_OPENAI_API_KEY', 'key\nBAD=value'), ('RICE_OPENAI_API_KEY', 'key\rBAD=value'), ('RICE_ANALYSIS_MODEL', 'another-model'), ('RICE_ANALYSIS_PASSES', '2'), ('RICE_ANALYSIS_ENABLED', 'false')])
def test_unsafe_or_unrequested_settings_rejected(field, value):
    settings = config()
    settings[field] = value
    with pytest.raises(ValueError):
        helper.validate_config(settings)


def test_load_only_analysis_fields_and_reject_duplicate(tmp_path):
    path = tmp_path / 'synthetic.env'
    text = 'OTHER=invalid unrelated " quote\n' + '\n'.join(k + '=' + v for k, v in config().items())
    path.write_text(text)
    assert helper.load_config(path) == config()
    path.write_text(text + '\nRICE_ANALYSIS_PASSES=1\n')
    with pytest.raises(ValueError):
        helper.load_config(path)


def test_guard_must_protect_upload_job_creation():
    guarded = '''
async def upload_photo():
    if ANALYSIS_ENABLED and request.headers.get("x-rice-archive-import", "").lower() != "true":
        conn.execute("INSERT INTO analysis_jobs VALUES(?)")
'''
    assert helper.archive_guard_present(guarded)
    assert not helper.archive_guard_present(guarded.replace(' and request.headers.get("x-rice-archive-import", "").lower() != "true"', ''))
    assert not helper.archive_guard_present(guarded.replace('INSERT INTO analysis_jobs', 'SELECT 1'))


def test_remote_program_compiles_without_plaintext_secret_or_network_call():
    program = helper.remote_program(config('synthetic-private-test-value'))
    compile(program, '<remote activation>', 'exec')
    assert 'synthetic-private-test-value' not in program
    assert 'api.openai.com' not in program
    assert "INSERT INTO analysis_jobs" in program  # guard inspection only
    assert "connection.execute(\"SELECT count(*) FROM analysis_jobs" in program
