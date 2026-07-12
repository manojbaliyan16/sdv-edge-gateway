# systemd units — Pi deployment

## can-interfaces.service

Brings `can0` and `can1` up at 500kbps automatically on every Pi boot, so the
gateway doesn't depend on someone SSHing in and running `ip link set` by hand
after a reboot or power loss.

Why this exists: on 12-Jul-26 the Pi rebooted after a power trip and the CAN
interfaces came back down. A `.bashrc` entry was considered first and
rejected — it only runs on interactive login shells, so it wouldn't fire on
an unattended boot, and it would re-prompt for sudo on every new terminal.
systemd runs it once, unattended, as root, at boot.

### Install

```bash
sudo cp deploy/systemd/can-interfaces.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable can-interfaces.service
sudo systemctl start can-interfaces.service
```

### Verify

```bash
systemctl status can-interfaces.service   # expect: active (exited), all ExecStart lines status=0/SUCCESS
ip -details -statistics link show can0    # expect: state ERROR-ACTIVE, bitrate 500000
ip -details -statistics link show can1
```

`active (exited)` is correct and expected for a `Type=oneshot` service with
`RemainAfterExit=yes` — it means the interfaces were brought up successfully
and there's no long-running process to keep alive. `failed` is the signal to
worry about, not `exited`.
