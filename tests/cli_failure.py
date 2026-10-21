"""A deliberate CLI rejection must unwind normally, never pass via a crash."""
import signal
import subprocess


def rejected(result, expected=None):
    def text(value):
        return value.decode(errors='replace') if isinstance(value,bytes) else value or ''
    diagnostic=text(result.stderr)
    output=text(result.stdout)+diagnostic
    assert result.returncode==2,(result.returncode,output)
    assert 'result: INVALID' in diagnostic,output
    assert not any(s in output for s in ('ThreadSanitizer:','AddressSanitizer:','runtime error:')),output
    if expected:assert expected in diagnostic,diagnostic


def check_predicate():
    rejected(subprocess.CompletedProcess([],2,b'',b'expected failure\nresult: INVALID\n'))
    cases=[(-s,b'') for s in (signal.SIGBUS,signal.SIGSEGV,signal.SIGABRT)]
    cases += [(0,b'result: INVALID'),(1,b'result: INVALID'),(2,b''),
              (2,b'ThreadSanitizer: data race\nresult: INVALID')]
    for status,diagnostic in cases:
        try:rejected(subprocess.CompletedProcess([],status,b'',diagnostic))
        except AssertionError:pass
        else:raise AssertionError('rejection predicate accepted a crash or wrong diagnostic')
