import sys

import frida


SOURCE = r"""
function watch(name, enter, leave) {
  Interceptor.attach(Module.getGlobalExportByName(name), {
    onEnter(args) { this.detail = enter(args); },
    onLeave(result) { leave(this, result); }
  });
}

const aimHandles = new Set();
const app = Process.findModuleByName('RaceStudio2.exe');
if (app !== null) {
  for (const [offset, label] of [[0x61220, 'AIM open'], [0x36aba0, 'AIM list']]) {
    Interceptor.attach(app.base.add(offset), {
      onEnter(args) { send(label + ' called'); },
      onLeave(result) { send(label + ' => ' + result); }
    });
  }
}

watch('SetupDiGetClassDevsA', args => {
  send('GetClassDevs flags=' + args[3] + ' guid=' + args[0]);
}, (context, result) => send('GetClassDevs => ' + result + ' error=' + context.lastError));
watch('SetupDiGetClassDevsW', args => {
  send('GetClassDevsW flags=' + args[3]);
}, (context, result) => send('GetClassDevsW => ' + result + ' error=' + context.lastError));

watch('SetupDiEnumDeviceInterfaces', args => {
  send('EnumDeviceInterfaces index=' + args[3].toInt32());
}, (context, result) => send('EnumDeviceInterfaces => ' + result + ' error=' + context.lastError));

watch('SetupDiGetDeviceInterfaceDetailA', args => args[2], (context, result) => {
  send('GetInterfaceDetail => ' + result + ' error=' + context.lastError +
       (!result.isNull() && !context.detail.isNull()
         ? ' path=' + context.detail.add(4).readAnsiString() : ''));
});

watch('CreateFileA', args => {
  const path = args[0].readAnsiString();
  if (/VID_11CC|BULK_|AIM_USB/i.test(path)) {
    send('CreateFileA ' + path);
    return path;
  }
  return null;
}, (context, result) => {
  if (context.detail) {
    send('CreateFileA => ' + result + ' error=' + context.lastError);
    if (result.toInt32() !== -1) aimHandles.add(result.toString());
  }
});

watch('CreateFileW', args => {
  const path = args[0].readUtf16String();
  if (/VID_11CC|BULK_|AIM_USB/i.test(path)) {
    send('CreateFileW ' + path);
    return path;
  }
  return null;
}, (context, result) => {
  if (context.detail) {
    send('CreateFileW => ' + result + ' error=' + context.lastError);
    if (result.toInt32() !== -1) aimHandles.add(result.toString());
  }
});

for (const name of ['ReadFile', 'WriteFile']) {
  watch(name, args => {
    if (!aimHandles.has(args[0].toString())) return null;
    send(name + ' AIM length=' + args[2].toUInt32());
    return true;
  }, (context, result) => {
    if (context.detail) send(name + ' AIM => ' + result + ' error=' + context.lastError);
  });
}

watch('DeviceIoControl', args => {
  const code = args[1].toUInt32();
  send('DeviceIoControl code=' + args[1]);
  if (code === 0x220010 && !args[2].isNull()) {
    const wide = args[3].toUInt32() === 56;
      const buffer = args[2].add(0x10).readPointer();
    send('bulk endpoint=' + args[2].add(4).readU32() +
         ' direction=' + args[2].add(8).readU32() +
        ' requested=' + args[2].add(wide ? 0x18 : 0x14).readU32() +
        ' buffer=' + buffer);
      return { bulk: true, output: args[4], buffer, wide };
  }
  if (code === 0x22000c) {
    const inputLength = args[3].toUInt32();
    send('vendor lengths: input=' + inputLength + ' output=' + args[5].toUInt32());
    if (!args[2].isNull() && inputLength) {
      send('vendor input:\n' + hexdump(args[2], { length: Math.min(inputLength, 64) }));
      const buffer = args[2].add(0x10).readPointer();
      const length = args[2].add(0x18).readU32();
      if (!buffer.isNull() && length <= 64)
        send('vendor payload before:\n' + hexdump(buffer, { length }));
      return { output: args[4], outputLength: args[5].toUInt32(), buffer, length };
    }
  }
}, (context, result) => {
  send('DeviceIoControl => ' + result + ' error=' + context.lastError);
  if (context.detail && context.detail.bulk) {
    const { output, buffer, wide } = context.detail;
    if (!output.isNull()) {
      const transferred = output.add(wide ? 0x20 : 0x1c).readU32();
      send('bulk transferred=' + transferred +
           ' completed=' + output.add(wide ? 0x2c : 0x28).readU32());
      if (result.toInt32() !== 0 && transferred > 0 && transferred <= 32 && !buffer.isNull())
        send('short bulk payload:\n' + hexdump(buffer, { length: transferred }));
    }
    return;
  }
  if (context.detail && result.toInt32() !== 0) {
    const { output, outputLength, buffer, length } = context.detail;
    if (!output.isNull() && outputLength)
      send('vendor output:\n' + hexdump(output, { length: Math.min(outputLength, 64) }));
    if (!buffer.isNull() && length <= 64)
      send('vendor payload after:\n' + hexdump(buffer, { length }));
  }
});
"""


def on_message(message, data):
    print(message.get("payload", message), flush=True)


spawned = sys.argv[1] == "--spawn"
pid = frida.spawn(sys.argv[2:]) if spawned else int(sys.argv[1])
session = frida.attach(pid)
try:
    script = session.create_script(SOURCE)
    script.on("message", on_message)
    script.load()
    if spawned:
        frida.resume(pid)
    print(f"TRACE READY: pid={pid}; press Enter to stop", flush=True)
    input()
finally:
    session.detach()