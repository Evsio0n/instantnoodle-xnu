#!/usr/bin/env python3
"""tools/bootq_test.py: bootq against a pretend phone, in a temporary folder. It never touches the real one."""
import fcntl, json, os, signal, subprocess, sys, tempfile, time

BOOTQ = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'bootq')
failed = 0


def check(name, ok, detail=''):
    global failed
    print(('ok   ' if ok else 'FAIL ') + name + ((': ' + str(detail)) if not ok else ''))
    failed += not ok


with tempfile.TemporaryDirectory() as tmp:
    logs = os.path.join(tmp, 'logs')
    os.makedirs(logs)
    # (BOOTQ_GETVAR and BOOTQ_STOP answer for the phone: nothing here may reach the real one)
    env = dict(os.environ, BOOTQ_DIR=os.path.join(tmp, 'q'), BOOTQ_PHONE=os.path.join(tmp, 'phone'), BOOTQ_LOGS=logs,
               BOOTQ_GETVAR='echo result: returned', BOOTQ_STOP='true')
    env.pop('BOOTQ_TICKET', None)
    order = os.path.join(tmp, 'order')

    def start(who, name, secs=1, boots=True, extra=()):
        # a pretend boot: says when it ran, leaves a run log named for itself, as long as it takes
        body = 'echo %s >> %s; sleep %s' % (name, order, secs)
        if boots:
            body += '; touch %s/%s-run.log' % (logs, name)
        return subprocess.Popen([BOOTQ, 'run', '--who', who, '--what', name, *extra, '--', body], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def status():
        return json.loads(subprocess.run([BOOTQ, 'status', '--json'], env=env, capture_output=True, text=True).stdout)

    def ran():
        with open(order) as f:
            return f.read().split()

    def wait_until(cond, secs=10):
        end = time.time() + secs
        while time.time() < end:
            if cond():
                return True
            time.sleep(0.1)
        return False

    a = start('metal', 'a', 2)
    wait_until(lambda: status()['running'])
    b = start('nf', 'b')
    time.sleep(0.3)
    c = start('metal', 'c')
    time.sleep(0.3)
    d = start('owner', 'd', extra=['--owner'])
    for p in (a, b, c, d):
        p.wait(30)
    check('one at a time, the owner next, then in order', ran() == ['a', 'd', 'b', 'c'], ran())
    check('each exits 0', all(p.returncode == 0 for p in (a, b, c, d)), [p.returncode for p in (a, b, c, d)])

    e = start('metal', 'e', 0, boots=False)
    out = e.communicate(timeout=20)[0]
    check('no new run log is 75, not success', e.returncode == 75, e.returncode)
    check('...and says it did not boot', 'did not boot' in out, out)

    f = start('metal', 'f', 0, boots=False, extra=['--no-boot-ok'])
    f.wait(20)
    check('--no-boot-ok is 0 with no run log', f.returncode == 0, f.returncode)

    g = start('metal', 'g', 0)
    out = g.communicate(timeout=20)[0]
    check('the run is named', 'run g' in out, out)

    # a run's name is its time of day: another day's log with the same name is not this run's, and does not hide it
    for old in ('same', 'arch'):
        with open(os.path.join(logs, old + '-run.log'), 'w') as w:
            w.write('another day')
        os.utime(os.path.join(logs, old + '-run.log'), (1000, 1000))
    p = start('metal', 'same', 0, boots=False)
    p.wait(20)
    check('an old log of that name, untouched, is still no boot', p.returncode == 75, p.returncode)
    p = start('metal', 'same', 0)
    out = p.communicate(timeout=20)[0]
    check('a run under an old log\'s name is seen', p.returncode == 0 and 'run same' in out, out)
    p = subprocess.Popen([BOOTQ, 'run', '--who', 'metal', '--what', 'arch', '--', 'touch %s/arch-run.log.live.tar.zst' % logs], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    p.wait(20)
    check('an older run being archived meanwhile is not this run', p.returncode == 75, p.returncode)

    # a boot started without bootq holds xl's lock: wait for it, and say so
    with open(env['BOOTQ_PHONE'] + '.who', 'w') as w:
        w.write('someone else')
    held = open(env['BOOTQ_PHONE'] + '.run', 'a')
    fcntl.flock(held, fcntl.LOCK_EX)
    n = len(ran())
    h = start('metal', 'h', 0)
    time.sleep(2.5)
    check('xl lock held outside the queue: it waits', len(ran()) == n, ran())
    check('status shows the phone busy', not status()['phone_free'])
    fcntl.flock(held, fcntl.LOCK_UN)
    held.close()
    out = h.communicate(timeout=20)[0]
    check('...runs once it is free', h.returncode == 0 and ran()[-1] == 'h', ran())
    check('...and said who had it', 'someone else' in out, out)

    # cancel: only a waiting ticket
    i = start('metal', 'i', 3)
    wait_until(lambda: status()['running'])
    j = start('nf', 'j')
    wait_until(lambda: status()['waiting'])
    jid = status()['waiting'][0]['id']
    iid = status()['running'][0]['id']
    r = subprocess.run([BOOTQ, 'cancel', str(iid)], env=env, capture_output=True, text=True)
    check('cancel leaves a running ticket alone', r.returncode == 1 and 'leaves it alone' in r.stdout, r.stdout)
    subprocess.run([BOOTQ, 'cancel', str(jid)], env=env, capture_output=True, text=True)
    j.wait(10)
    check('cancel takes a waiting ticket out', j.returncode == 125, j.returncode)
    i.wait(20)
    check('...and it never ran', 'j' not in ran(), ran())
    check('the running one went on', i.returncode == 0 and 'i' in ran(), i.returncode)

    # a ticket whose process was killed must not block the queue
    k = start('metal', 'k', 30)
    wait_until(lambda: status()['running'])
    os.kill(k.pid, signal.SIGKILL)
    k.wait(5)
    m = start('nf', 'm', 0)
    m.wait(30)
    check('a killed ticket does not block the next', m.returncode == 0 and ran()[-1] == 'm', ran())

    # --wait gives up
    n1 = start('metal', 'n1', 4)
    wait_until(lambda: status()['running'])
    n2 = start('nf', 'n2', 0, extra=['--wait', '1'])
    n2.wait(15)
    check('--wait gives up with 124', n2.returncode == 124, n2.returncode)
    n1.wait(20)

    # the owner's click stops a session's run, and only with --preempt
    flag = os.path.join(tmp, 'stopped')
    env['BOOTQ_STOP'] = 'touch ' + flag
    s1 = subprocess.Popen([BOOTQ, 'run', '--who', 'metal', '--what', 's1', '--',
                           'for i in 1 2 3 4 5 6 7 8 9 10; do [ -f %s ] && break; sleep 1; done; echo s1 >> %s; touch %s/s1-run.log' % (flag, order, logs)],
                          env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    wait_until(lambda: status()['running'])
    began = time.time()
    s2 = start('owner', 's2', 0, extra=['--owner', '--preempt'])
    s2.wait(20)
    s1.wait(20)
    check('--preempt stops the running one, and the owner goes next', ran()[-2:] == ['s1', 's2'] and os.path.exists(flag) and time.time() - began < 8, ran())

    # a stop that comes before the kernel is up does nothing: the click tries again
    stops = os.path.join(tmp, 'stops')
    env['BOOTQ_STOP'] = 'echo stop >> ' + stops
    s3 = subprocess.Popen([BOOTQ, 'run', '--who', 'metal', '--what', 's3', '--',
                           'for i in $(seq 1 30); do [ "$(cat %s 2>/dev/null | wc -l)" -ge 2 ] && break; sleep 1; done; echo s3 >> %s; touch %s/s3-run.log' % (stops, order, logs)],
                          env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    wait_until(lambda: status()['running'])
    began = time.time()
    s4 = start('owner', 's4', 0, extra=['--owner', '--preempt'])
    s4.wait(45)
    s3.wait(45)
    check('the click stops again until the run has gone', ran()[-2:] == ['s3', 's4'] and time.time() - began < 25, (ran()[-2:], time.time() - began))

    # a kernel left running with xl's lock free: nothing starts until it has gone, and status and the ender say so
    answer = os.path.join(tmp, 'answer')
    with open(answer, 'w') as w:
        w.write('result: returned')
    env['BOOTQ_GETVAR'] = 'cat ' + answer
    o1 = subprocess.Popen([BOOTQ, 'run', '--who', 'nf', '--what', 'o1', '--', 'echo "result: running" > %s; exit 2' % answer],
                          env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out = o1.communicate(timeout=20)[0]
    check('a command that leaves a kernel running is told so', 'still running' in out, out)
    n = len(ran())
    o2 = start('metal', 'o2', 0)
    time.sleep(2.5)
    check('the next ticket waits for it', len(ran()) == n and o2.poll() is None, ran())
    r = subprocess.run([BOOTQ, 'status'], env=env, capture_output=True, text=True)
    check('status says a kernel is running', 'nobody is watching' in r.stdout, r.stdout)
    with open(answer, 'w') as w:
        w.write('result: returned')
    out = o2.communicate(timeout=20)[0]
    check('it goes on when the kernel has come back', o2.returncode == 0 and ran()[-1] == 'o2', ran())
    check('...having said why it waited', 'nobody is watching' in out, out)

    # rests: a session waits them out, the owner does not
    env['BOOTQ_COOL_SHARE'] = '2'
    subprocess.run([BOOTQ, 'cool', 'on'], env=env, capture_output=True, text=True)
    c1 = start('metal', 'c1', 2)
    c1.wait(20)
    n = len(ran())
    c2 = start('nf', 'c2', 0)
    time.sleep(1.5)
    check('a rest holds the next session ticket', len(ran()) == n and 'cooling' in subprocess.run([BOOTQ, 'status'], env=env, capture_output=True, text=True).stdout, ran())
    check('--json says how long the rest is', status()['cooling'] > 0, status())
    c3 = start('owner', 'c3', 0, extra=['--owner'])
    c3.wait(10)
    check('...but not the owner', c3.returncode == 0 and ran()[-1] == 'c3', ran())
    c2.wait(20)
    check('...and it goes on after', c2.returncode == 0 and ran()[-1] == 'c2', ran())
    subprocess.run([BOOTQ, 'cool', 'off'], env=env, capture_output=True, text=True)
    r = subprocess.run([BOOTQ, 'status'], env=env, capture_output=True, text=True)
    check('off ends the rest', 'free' in r.stdout, r.stdout)

    # a run that hung, and one that needs the owner's hands
    p = subprocess.Popen([BOOTQ, 'run', '--who', 'metal', '--what', 'h1', '--', 'echo "THE KERNEL HUNG on core 700" > %s/h1-run.log' % logs], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out = p.communicate(timeout=20)[0]
    check('a hung run is not a success', p.returncode == 76 and 'HUNG' in out and not status()['cooling'], (p.returncode, out))
    p = subprocess.Popen([BOOTQ, 'run', '--who', 'metal', '--what', 'h2', '--', 'echo "loader return halted, cold recovery required" > %s/h2-run.log; echo "THE KERNEL HUNG" >> %s/h2-run.log' % (logs, logs)], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out = p.communicate(timeout=20)[0]
    check('cold recovery holds the sessions', p.returncode == 76 and 'cold recovery' in out and status()['cooling'] > 1000, (p.returncode, status()['cooling']))
    n = len(ran())
    h3 = start('nf', 'h3', 0)
    time.sleep(1.5)
    check('...their tickets wait', len(ran()) == n and h3.poll() is None, ran())
    subprocess.run([BOOTQ, 'cool', 'off'], env=env, capture_output=True, text=True)
    h3.wait(20)
    check('...until cool off', h3.returncode == 0 and ran()[-1] == 'h3', (h3.returncode, ran()[-1:]))

    # a ticket inside a ticket just runs
    env2 = dict(env, BOOTQ_TICKET='9')
    r = subprocess.run([BOOTQ, 'run', '--who', 'x', '--what', 'y', '--', 'echo inner'], env=env2, capture_output=True, text=True)
    check('inside a ticket, run just runs', r.stdout.strip() == 'inner' and r.returncode == 0, r.stdout)

    r = subprocess.run([BOOTQ, 'log', '50'], env=env, capture_output=True, text=True)
    check('the log has the ended ones', 'state' not in r.stdout and 'cancelled' in r.stdout and 'died' in r.stdout, r.stdout)

print('FAILED %d' % failed if failed else 'all passed')
sys.exit(1 if failed else 0)
