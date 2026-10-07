"""Actual shell flows with isolated fake Proxmox commands; never provision a CT."""
import json
import os
import pty
import select
import subprocess
import sys
import tarfile
import termios
import time
import fcntl
from pathlib import Path

import pytest

LXC = Path(__file__).parents[2]
PYTHON = sys.executable

FAKE = r'''#!/usr/bin/env python3
import json, os, sys, tarfile, shutil, time
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['CALLS'], 'a') as f: f.write(json.dumps([name] + args) + '\n')
fail = os.environ.get('FAIL', '')
if name == 'id': print('0'); sys.exit()
if name == 'sleep': sys.exit()
if name == 'whiptail':
    if fail == 'dialog-cancel': sys.exit(1)
    answer = sys.stdin.readline().rstrip('\n') or args[args.index('--output-fd')-1]
    os.write(1,b'Dialog screen rendering\n')
    os.write(int(args[args.index('--output-fd')+1]),answer.encode()); sys.exit()
if name == 'pvesh':
    if any(x.endswith('/storage') for x in args):
        print(json.dumps([{'storage':'ct-fast','active':1,'content':'rootdir,images'}, {'storage':'templates','active':1,'content':'vztmpl,iso'}, {'storage':'offline','active':0,'content':'rootdir,vztmpl'}, {'storage':'images-only','active':1,'content':'images'}]))
    elif any(x.endswith('/network') for x in args): print(json.dumps([{'iface':'vmbr7','type':'bridge','active':1}]))
    elif '/cluster/nextid' in args: print('231')
    elif '/cluster/resources' in args: print(json.dumps([{'vmid':230,'type':'qemu'},{'vmid':232,'type':'lxc'}]))
    elif '/nodes' in args: print(json.dumps([{'node':'pve-test','status':'online'}]))
    sys.exit()
if name == 'hostname': print('pve-test'); sys.exit()
if name == 'pveam':
    if args[0] == 'available' and fail == 'unsafe-template': print('system debian-13-standard_../../escape.tar.zst'); sys.exit()
    if args[0] == 'available' and fail != 'template': print('system debian-13-standard_13.1-1_amd64.tar.zst')
    if args[0] == 'list' and fail != 'missing-download' and Path(os.environ['CALLS'] + '.download').exists(): print('templates:vztmpl/debian-13-standard_13.1-1_amd64.tar.zst 123')
    if args[0] == 'download': Path(os.environ['CALLS'] + '.download').touch()
    sys.exit()
if name == 'pct':
    if args[0] == 'create' and fail == 'create': sys.exit(1)
    if args[0] == 'start' and fail == 'start': sys.exit(1)
    if args[0] == 'push':
        if args[2].endswith('.tar.gz'):
            with tarfile.open(args[2]) as archive:
                Path(os.environ['PAYLOAD']).write_text(json.dumps(archive.getnames()))
    if args[0] == 'exec':
        command = ' '.join(args[3:])
        if 'install-server.sh' in command and fail == 'interrupt-install': time.sleep(5)
        if 'apt-get' in command and fail == 'update': sys.exit(1)
        if 'install-server.sh' in command and fail == 'install': sys.exit(1)
        if 'systemctl is-active' in command and fail == 'service': sys.exit(1)
        if 'getent hosts' in command and fail == 'internet': sys.exit(1)
        if 'curl' in command and fail in ('internet', 'health'): sys.exit(1)
        if 'ip -4' in command: print('2: eth0 inet ' + os.environ.get('ASSIGNED_IP','192.168.7.41') + '/24 scope global eth0')
    sys.exit()
if name == 'curl':
    if fail == 'download': sys.exit(22)
    if any('raw.githubusercontent.com' in x for x in args):
        print(Path(os.environ['BOOTSTRAP_SOURCE']).read_text()); sys.exit()
    dest = args[args.index('--output')+1]
    shutil.copyfile(os.environ['REPO_ARCHIVE'], dest)
    sys.exit()
sys.exit(99)
'''

@pytest.fixture
def host(tmp_path):
    commands = tmp_path / 'bin'
    commands.mkdir()
    for name in ('id','pvesh','hostname','pveam','pct','sleep','curl'):
        target = commands / name
        target.write_text(FAKE.replace('#!/usr/bin/env python3', f'#!{PYTHON}'))
        target.chmod(0o755)
    env = dict(os.environ, PATH=f'{commands}:{os.environ["PATH"]}', CALLS=str(tmp_path/'calls'), PAYLOAD=str(tmp_path/'payload'), TMPDIR=str(tmp_path))
    return tmp_path, env


def run_tty(script, env, answers, args=(), streamed=False, interrupt_on=None):
    master, slave = pty.openpty()
    def setup():
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    command = ['bash','-c','bash -s -- "$@" < "$0"',str(script),*args] if streamed else ['bash',str(script),*args]
    process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave, env=env, preexec_fn=setup)
    os.close(slave)
    os.write(master, answers.encode())
    output = bytearray()
    deadline = time.monotonic()+15
    while time.monotonic() < deadline:
        if select.select([master], [], [], .1)[0]:
            try: output.extend(os.read(master, 65536))
            except OSError: break
        if interrupt_on and interrupt_on.encode() in output:
            os.write(master,b'\x03')
            interrupt_on = None
        if process.poll() is not None: break
    if process.poll() is None:
        process.kill()
    code = process.wait()
    os.close(master)
    return code, output.decode(errors='replace')


def calls(host):
    target = host[0]/'calls'
    return [json.loads(line) for line in target.read_text().splitlines()] if target.exists() else []


def defaults(confirm='yes'):
    # CTID, hostname, cores, RAM, swap, disk, root storage, template storage,
    # bridge, IP, gateway, DNS, language, confirmation.
    return '\n'*13 + confirm + '\n'


def test_interactive_install_full_payload_and_health(host):
    code, output = run_tty(LXC/'scripts/deploy-proxmox.sh', host[1], defaults())
    assert code == 0, output
    invoked = calls(host)
    creation = next(c for c in invoked if c[:2] == ['pct','create'])
    assert creation[2:4] == ['231','templates:vztmpl/debian-13-standard_13.1-1_amd64.tar.zst']
    assert creation[creation.index('--rootfs')+1] == 'ct-fast:16'
    assert creation[creation.index('--unprivileged')+1] == '1'
    assert 'bridge=vmbr7,ip=dhcp' in creation[creation.index('--net0')+1]
    assert any('apt-get update' in ' '.join(c) and 'upgrade' in ' '.join(c) for c in invoked)
    assert any('systemctl is-active' in ' '.join(c) for c in invoked)
    assert any('http://192.168.7.41:8080/health' in ' '.join(c) for c in invoked)
    assert 'http://192.168.7.41:8080' in output
    assert not any('API_TOKEN=' in arg for c in invoked for arg in c)
    payload = json.loads((host[0]/'payload').read_text())
    for path in ('server/app.py','server/requirements.txt','server/web/index.html','server/web/app.css','server/web/app.js','systemd/voice-notes-api.service','scripts/install-server.sh'):
        assert path in payload
    assert not any(c[:2] == ['pct','destroy'] for c in invoked)
    for phase in ('discovery','validation','template','create','start','readiness','update','payload','install','service','address','health'):
        assert f'Phase: {phase}' in output


@pytest.mark.parametrize('index,value', [(0,'230'),(0,'232'),(0,'99'),(1,'bad,host'),(2,'0'),(2,'257'),(3,'127'),(4,'-1'),(5,'1'),(6,'images-only'),(6,'offline'),(7,'ct-fast'),(8,'vmbr0'),(9,'bad/24'),(9,'127.0.0.1/24'),(10,'1.2.3.4'),(11,'1.2.999.4'),(12,'xx')])
def test_invalid_input_stops_before_mutation(host, index, value):
    answers = ['']*13 + ['yes']
    answers[index] = value
    code, output = run_tty(LXC/'scripts/deploy-proxmox.sh',host[1],'\n'.join(answers)+'\n')
    assert code != 0, output
    assert 'Invalid' in output or 'already exists' in output, output
    assert not any(c[0] in ('pct','pveam') for c in calls(host))


def test_declining_confirmation_is_read_only(host):
    code, output = run_tty(LXC/'scripts/deploy-proxmox.sh',host[1],defaults('no'))
    assert code != 0
    assert 'Aborted' in output
    assert not any(c[0] in ('pct','pveam') for c in calls(host))


def test_static_network_selection(host):
    answers = ['231','notes-1','2','2048','0','8','ct-fast','templates','vmbr7','192.168.7.55/24','192.168.7.1','9.9.9.9','en','yes']
    code, output = run_tty(LXC/'scripts/deploy-proxmox.sh',host[1],'\n'.join(answers)+'\n')
    assert code == 0, output
    creation = next(c for c in calls(host) if c[:2] == ['pct','create'])
    assert 'name=eth0,bridge=vmbr7,ip=192.168.7.55/24,gw=192.168.7.1' in creation
    assert creation[creation.index('--nameserver')+1] == '9.9.9.9'


@pytest.mark.parametrize('failure', ['template','unsafe-template','missing-download','create','start','internet','update','install','service','health'])
def test_failure_keeps_container_and_never_claims_success(host, failure):
    env = dict(host[1], FAIL=failure)
    code, output = run_tty(LXC/'scripts/deploy-proxmox.sh',env,defaults())
    assert code != 0, output
    assert 'deployed and health verified' not in output
    assert 'Failed phase:' in output and 'CTID: 231' in output
    invoked = calls(host)
    assert not any(c[:2] in (['pct','destroy'],['pct','stop']) for c in invoked)
    if failure in ('template','unsafe-template','missing-download'):
        assert not any(c[:2] == ['pct','create'] for c in invoked)
    assert not list(host[0].glob('tmp.*'))


def test_payload_contains_only_runtime_files(host):
    code, output = run_tty(LXC/'scripts/deploy-proxmox.sh',host[1],defaults())
    assert code == 0, output
    payload = json.loads((host[0]/'payload').read_text())
    assert set(payload) == {'server/app.py','server/requirements.txt','server/web/index.html','server/web/app.css','server/web/app.js','systemd/voice-notes-api.service','scripts/install-server.sh'}


@pytest.mark.parametrize('ip', ['127.0.0.1','169.254.1.2','0.0.0.0'])
def test_invalid_assigned_address_never_prints_url(host, ip):
    code, output = run_tty(LXC/'scripts/deploy-proxmox.sh',dict(host[1],ASSIGNED_IP=ip),defaults())
    assert code != 0
    assert 'API URL:' not in output


def test_explicit_unattended_env_file_without_tty(host):
    env_file = host[0]/'settings.env'
    env_file.write_text('VMID=231\nHOSTNAME=voice-notes-stt\nSTORAGE=ct-fast\nTEMPLATE_STORAGE=templates\nBRIDGE=vmbr7\nAPI_TOKEN=synthetic-test-token\nWHISTLE_LANGUAGE=fr\n')
    result = subprocess.run(['bash',str(LXC/'scripts/deploy-proxmox.sh'),'--unattended','--env-file',str(env_file)],env=host[1],capture_output=True,text=True)
    assert result.returncode == 0, result.stderr
    assert 'synthetic-test-token' not in result.stdout+result.stderr
    assert not any('synthetic-test-token' in ' '.join(c) for c in calls(host))


def test_unattended_env_is_data_not_shell_code(host):
    env_file = host[0]/'settings.env'
    marker = host[0]/'executed'
    env_file.write_text(f'VMID=231\nHOSTNAME=$(touch {marker})\n')
    result = subprocess.run(['bash',str(LXC/'scripts/deploy-proxmox.sh'),'--unattended','--env-file',str(env_file)],env=host[1],capture_output=True,text=True)
    assert result.returncode != 0
    assert not marker.exists()
    assert not any(c[0] in ('pct','pveam') for c in calls(host))


def repo_archive(host):
    archive = host[0]/'repo.tar.gz'
    with tarfile.open(archive,'w:gz') as target:
        for path in ('scripts/deploy-proxmox.sh','scripts/install-server.sh','server/app.py','server/requirements.txt','server/web/index.html','server/web/app.css','server/web/app.js','systemd/voice-notes-api.service'):
            target.add(LXC/path,arcname='esp32EPaperNote-main/lxc/'+path)
    return dict(host[1],REPO_ARCHIVE=str(archive))


def test_bootstrap_downloads_archive_and_executes_complete_local_installer(host):
    code, output = run_tty(LXC/'scripts/bootstrap-proxmox.sh',repo_archive(host),defaults())
    assert code == 0, output
    download = next(c for c in calls(host) if c[0] == 'curl')
    assert 'https://github.com/maelremrem/esp32EPaperNote/archive/refs/heads/main.tar.gz' in download
    assert '--fail' in download and '--max-time' in download and '--proto' in download
    assert 'health verified' in output
    assert not list(host[0].glob('voice-notes-bootstrap.*'))


def test_bootstrap_failed_download_never_executes_installer(host):
    code, output = run_tty(LXC/'scripts/bootstrap-proxmox.sh',dict(repo_archive(host),FAIL='download'),defaults())
    assert code != 0
    assert any(c[0] == 'curl' for c in calls(host))
    assert not any(c[0] in ('pvesh','pct','pveam') for c in calls(host))
    assert not list(host[0].glob('voice-notes-bootstrap.*'))


@pytest.mark.parametrize('revision', ['a'*40,'../main','main;id'])
def test_bootstrap_revision_is_bounded(host, revision):
    env = repo_archive(host)
    code, output = run_tty(LXC/'scripts/bootstrap-proxmox.sh',env,defaults(),('--revision',revision))
    if revision == 'a'*40:
        assert code == 0, output
        assert f'https://github.com/maelremrem/esp32EPaperNote/archive/{revision}.tar.gz' in next(c for c in calls(host) if c[0]=='curl')
    else:
        assert code != 0
        assert not calls(host)


@pytest.mark.parametrize('kind', ['traversal','symlink','missing'])
def test_bootstrap_rejects_unsafe_or_incomplete_archives(host, kind):
    env = repo_archive(host)
    with tarfile.open(env['REPO_ARCHIVE'],'w:gz') as archive:
        if kind != 'missing':
            entry = tarfile.TarInfo('../escaped' if kind == 'traversal' else 'root/link')
            if kind == 'symlink': entry.type = tarfile.SYMTYPE; entry.linkname='/etc'
            archive.addfile(entry)
    code, output = run_tty(LXC/'scripts/bootstrap-proxmox.sh',env,defaults())
    assert code != 0
    assert not any(c[0] in ('pvesh','pct','pveam') for c in calls(host))
    assert not (host[0]/'escaped').exists()
    assert not list(host[0].glob('voice-notes-bootstrap.*'))


def test_bootstrap_streamed_stdin_uses_terminal_for_prompts(host):
    code, output = run_tty(LXC/'scripts/bootstrap-proxmox.sh',repo_archive(host),defaults(),streamed=True)
    assert code == 0, output
    assert 'health verified' in output


def test_interrupt_cleans_bootstrap_scratch_without_mutation(host):
    code, output = run_tty(LXC/'scripts/bootstrap-proxmox.sh',repo_archive(host),'',interrupt_on='Container ID')
    assert code != 0, output
    assert not list(host[0].glob('voice-notes-bootstrap.*'))
    assert not any(c[0] in ('pct','pveam') for c in calls(host))


def test_install_config_private_from_first_write(tmp_path):
    source = (LXC/'scripts/install-server.sh').read_text()
    section = source.split('API_TOKEN="${API_TOKEN:-}"',1)[1].split('# Cache the weights',1)[0]
    target = tmp_path/'config.env'
    observed = tmp_path/'mode'
    section = 'API_TOKEN="${API_TOKEN:-}"'+section
    section = section.replace('/etc/voice-notes.env',str(target))
    section = section.replace('chmod 600',f'stat -c %a {target} > {observed}\nchmod 600')
    env = dict(os.environ,API_TOKEN='test-only-token',WHISTLE_LANGUAGE='fr')
    result = subprocess.run(['bash','-c','umask 022\n'+section],env=env,capture_output=True,text=True)
    assert result.returncode == 0, result.stderr
    assert observed.read_text().strip() == '600'
    assert 'test-only-token' not in result.stdout+result.stderr


def test_install_does_not_advertise_url_before_outer_health_verification():
    source = (LXC/'scripts/install-server.sh').read_text()
    assert 'API URL:' not in source
    assert 'http://' not in source.split('cat <<INFO',1)[1]


@pytest.mark.parametrize('failed', [False, True])
def test_documented_one_liner_is_fail_closed(host, failed):
    command = next(line for line in (LXC/'README.md').read_text().splitlines() if line.startswith("bash -c 'set -e; script=$(curl"))
    wrapper = host[0]/'wrapper.sh'
    wrapper.write_text(command+'\n')
    env = dict(repo_archive(host),BOOTSTRAP_SOURCE=str(LXC/'scripts/bootstrap-proxmox.sh'),FAIL='download' if failed else '')
    code, output = run_tty(wrapper,env,defaults())
    if failed:
        assert code != 0
        assert [c[0] for c in calls(host)] == ['curl']
    else:
        assert code == 0, output
        assert 'health verified' in output


@pytest.mark.parametrize('cancel', [False, True])
def test_available_whiptail_is_used_and_cancel_is_read_only(host, cancel):
    fake = Path(host[1]['PATH'].split(':')[0])/'whiptail'
    fake.write_text(FAKE.replace('#!/usr/bin/env python3',f'#!{PYTHON}'))
    fake.chmod(0o755)
    env = dict(host[1],TERM='xterm',FAIL='dialog-cancel' if cancel else '')
    code, output = run_tty(LXC/'scripts/deploy-proxmox.sh',env,defaults())
    assert any(c[0]=='whiptail' for c in calls(host))
    if cancel:
        assert code != 0
        assert not any(c[0] in ('pct','pveam') for c in calls(host))
    else:
        assert code == 0, output
        confirmation = [c for c in calls(host) if c[0]=='whiptail'][-1]
        assert 'CTID=231' in ' '.join(confirmation)
        assert 'unprivileged=1' in ' '.join(confirmation)


def test_interrupt_after_creation_keeps_ct_and_cleans_only_scratch(host):
    keep = host[0]/'unrelated-user-file'
    keep.write_text('preserve')
    code, output = run_tty(LXC/'scripts/deploy-proxmox.sh',dict(host[1],FAIL='interrupt-install'),defaults(),interrupt_on='Phase: install')
    assert code != 0, output
    assert keep.read_text() == 'preserve'
    assert not list(host[0].glob('tmp.*'))
    assert any(c[:2] == ['pct','create'] for c in calls(host))
    assert not any(c[:2] in (['pct','destroy'],['pct','stop']) for c in calls(host))
    assert 'health verified' not in output
