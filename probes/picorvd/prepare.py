#!/usr/bin/env python3
"""Apply reviewed integration fixes to a generated copy of pinned PicoRVD."""
from pathlib import Path
import shutil
import sys


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Pinned PicoRVD source changed: ' + old[:80])
    return text.replace(old, new)


def replace_function(text, signature, body):
    start = text.index(signature + ' {')
    end = text.index('\n}', start) + 2
    return replace(text, text[start:end], signature + ' {\n' + body + '\n}')


def prepare(source, output):
    shutil.copytree(source / 'src', output, dirs_exist_ok=True)
    path = output / 'main.cpp'
    text = path.read_text()
    text = replace(text, '  while (1) {', '  while (1) {\n    watchdog_update();')
    text = replace(text, '  while(time_us_32() < (now + us));', '  while ((uint32_t)(time_us_32() - now) < (uint32_t)us);')
    text = replace(text, '#include "Console.h"', '#include "usb_bridge.h"')
    start = text.index('  printf_g("// Starting Console')
    end = text.index('  while (1)', start)
    text = replace(text, text[start:end], '')
    start = text.index('    bool connected = tud_cdc_n_connected(0);')
    end = text.index('    console->update(ser_ie, ser_in);', start) + len('    console->update(ser_ie, ser_in);')
    text = replace(text, text[start:end], '    service_gdb_usb(*gdb);')
    path.write_text(text)
    path = output / 'Packet.h'
    text = path.read_text()
    text = replace(text, 'if ((cursor2 - buf) + d > size)', 'if (d < 0 || d > size - (cursor2 - buf))')
    text = replace(text, '    int accum = 0;', '    uint32_t accum = 0;')
    text = text.replace('isspace(peek_char())', 'isspace(static_cast<unsigned char>(peek_char()))')
    text = replace(text, 'isspace(*c)', 'isspace(static_cast<unsigned char>(*c))')
    text = replace(text, '((cursor2 - buf) <= size - 2)', '((cursor2 - buf) <= this->size - 2)')
    text = replace(text, '    while (*p) {\n      auto hi', '    while (*p) {\n      if (c - buf > size - 2) return false;\n      auto hi')
    text = replace(text, '''    *cursor2++ = c;
    size++;''', '''    if (size >= static_cast<int>(sizeof(buf)) - 1) {
      error = true;
      return;
    }
    *cursor2++ = c;
    size++;
    *cursor2 = 0;''')
    path.write_text(text)
    path = output / 'GDBServer.h'
    text = replace(path.read_text(), 'char expected_checksum = 0;', 'uint8_t expected_checksum = 0;')
    text = replace(text, 'uint32_t last_halt_check;', 'uint32_t last_halt_check = 0;')
    path.write_text(text)
    path = output / 'GDBServer.cpp'
    text = path.read_text()
    text = replace(text, '(1 << page_offset)', '(1ull << page_offset)')
    text = replace(text, 'PacketSize=32768;', 'PacketSize=3fff;')
    text = replace(text, """  for(int i = 0; i < rvd->get_gpr_count(); i++) {
    rvd->set_gpr(i, recv.take_hex(8));
  }
  rvd->set_dpc(recv.take_hex(8));""", """  uint32_t values[17] = {};
  if (recv.size != 1 + 17 * 8) recv.error = true;
  for (int i = 0; i < 17 && !recv.error; ++i) values[i] = swap(recv.take_hex(8));
  if (!recv.error) {
    for (int i = 0; i < rvd->get_gpr_count(); ++i) rvd->set_gpr(i, values[i]);
    rvd->set_dpc(values[16]);
  }""")
    text = replace(text, """  int gpr = recv.take_hex();

  if (!recv.error) {""", """  int gpr = recv.take_hex();
  if (gpr < 0 || gpr > rvd->get_gpr_count() || recv.cursor2 != recv.buf + recv.size) recv.error = true;
  if (recv.error) send.set_packet("E01");

  if (!recv.error) {""")
    text = replace(text, """  unsigned int val = recv.take_hex();

  if (!recv.error) {""", """  unsigned int val = swap(recv.take_hex(8));
  if (gpr < 0 || gpr > rvd->get_gpr_count() || recv.cursor2 != recv.buf + recv.size) recv.error = true;
  if (recv.error) send.set_packet("E01");

  if (!recv.error) {""")

    text = replace(text, 'if (checksum != expected_checksum)', 'if (recv.error || checksum != expected_checksum)')
    text = replace(text, '''  if (recv.error) {
    LOG_R("\\nhandle_m''', '''  if (len < 0 || len > (static_cast<int>(sizeof(send.buf)) - 1) / 2) recv.error = true;
  if (recv.error) {
    LOG_R("\\nhandle_m''')
    # Validate the complete memory-write packet before any target-side mutation.
    text = replace(text, '''  if (recv.error) {
    LOG_R("\\nhandle_M''', '''  auto payload = recv.cursor2;
  auto remaining = recv.size - (payload - recv.buf);
  if (len > static_cast<uint32_t>(remaining / 2) || len * 2 != remaining) recv.error = true;
  for (int i = 0; !recv.error && i < remaining; ++i)
    if (recv.from_hex(payload[i]) < 0) recv.error = true;
  if (recv.error) {
    LOG_R("\\nhandle_M''')
    text = replace(text, '''  recv.take('D');
  LOG''', '''  recv.take('D');
  soft->halt();
  soft->clear_all_breakpoints();
  soft->resume();
  LOG''')
    text = replace(text, '''      if (recv.error) {
        LOG_R("\\nBad vFlashErase''', '''      if (addr < 0 || size <= 0 || addr > flash->get_flash_size() ||
          size > flash->get_flash_size() - addr ||
          addr % flash->get_page_size() || size % flash->get_page_size()) recv.error = true;
      if (recv.error) {
        LOG_R("\\nBad vFlashErase''')
    text = replace(text, '''      while ((recv.cursor2 - recv.buf) < recv.size) {
        put_flash_cache(addr++, recv.take_char());
      }
      send.set_packet("OK");''', '''      int length = recv.size - (recv.cursor2 - recv.buf);
      if (recv.error || addr < 0 || addr > flash->get_flash_size() ||
          length > flash->get_flash_size() - addr) {
        send.set_packet("E01");
      } else {
        while ((recv.cursor2 - recv.buf) < recv.size)
          put_flash_cache(addr++, recv.take_char());
        send.set_packet("OK");
      }''')
    text = replace(text, 'else if (recv.match_prefix("qC")) {', '''else if (recv.match_prefix("qCRC:")) {
    uint32_t address = recv.take_hex();
    recv.take(',');
    uint32_t length = recv.take_hex();
    bool in_flash = address <= 0x4000u && length <= 0x4000u - address;
    bool in_ram = address >= 0x20000000u && address <= 0x20000800u && length <= 0x20000800u - address;
    if (recv.error || recv.cursor2 != recv.buf + recv.size || (!in_flash && !in_ram)) {
      send.set_packet("E01");
    } else {
      // GDB qCRC uses MSB-first CRC-32, seed all ones, no final inversion.
      uint32_t crc = 0xffffffffu;
      for (uint32_t i = 0; i < length; ++i) {
        crc ^= (uint32_t)rvd->get_mem_u8(address + i) << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
          crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
      }
      char reply[10];
      snprintf(reply, sizeof(reply), "C%08x", (unsigned)crc);
      send.set_packet(reply);
    }
  }
  else if (recv.match_prefix("qC") && recv.cursor2 == recv.buf + recv.size) {''')
    text = replace(text, '''        send.start_packet();
        send.put_str("l");
        send.put_str(memory_map);
        send.end_packet();''', '''        size_t total = strlen(memory_map);
        if (offset < 0 || length <= 0 || static_cast<size_t>(offset) > total) {
          send.set_packet("E01");
        } else {
          size_t count = total - offset;
          if (count > static_cast<size_t>(length)) count = length;
          if (count > sizeof(send.buf) - 2) count = sizeof(send.buf) - 2;
          send.start_packet();
          send.put(offset + count == total ? 'l' : 'm');
          for (size_t i = 0; i < count; ++i) send.put(memory_map[offset + i]);
          send.end_packet();
        }''')
    text = replace(text, '      next_state = SEND_PACKET;\n      break;\n    }\n\n    case SEND_SUFFIX1:', '''      send.cursor2++;
      next_state = (send.cursor2 - send.buf) == send.size ? SEND_SUFFIX1 : SEND_PACKET;
      break;
    }

    case SEND_SUFFIX1:''')
    text = replace(text, '          next_state = SEND_PACKET;\n        }\n        else {', '          next_state = SEND_PREFIX;\n        }\n        else {')
    # No stale partially buffered page may survive a lost debugger connection.
    text = replace(text, '    soft->clear_all_breakpoints();\n    soft->resume();',
                   '    soft->halt();\n    soft->clear_all_breakpoints();\n    soft->resume();\n    reset();')
    text = replace(text, '  int src = recv.take_hex();', '  uint32_t src = recv.take_hex();')
    text = replace(text, '''  if (recv.maybe_take_hex(addr)) {
    soft->set_dpc(addr);
  }''', '''  bool has_address = recv.cursor2 != recv.buf + recv.size;
  if (has_address) addr = recv.take_hex();
  if (recv.error || recv.cursor2 != recv.buf + recv.size) {
    send.set_packet("E01");
    next_state = SEND_PREFIX;
    return;
  }
  if (has_address) soft->set_dpc(addr);''')
    text = replace(text, "  recv.take('s');\n  soft->step();", '''  recv.take('s');
  bool has_address = recv.cursor2 != recv.buf + recv.size;
  uint32_t address = has_address ? recv.take_hex() : 0;
  if (recv.error || recv.cursor2 != recv.buf + recv.size) {
    send.set_packet("E01");
    next_state = SEND_PREFIX;
    return;
  }
  if (has_address) soft->set_dpc(address);
  soft->step();''')
    text = replace(text, 'if (recv.match_prefix_hex("reset")) {',
                   'if (recv.match_prefix_hex("reset") && recv.cursor2 == recv.buf + recv.size) {\n      reset();')
    text = replace(text, '  if (len < 0 || len >',
                   '  if (recv.cursor2 != recv.buf + recv.size ||\n      (len > 0 && static_cast<uint32_t>(len - 1) > UINT32_MAX - src)) recv.error = true;\n  if (len < 0 || len >')
    text = replace(text, '    if (len == 2) {', '    if (len == 2 && (src & 1u) == 0) {')
    text = replace(text, '    else if (len == 4) {', '    else if (len == 4 && (src & 3u) == 0) {')
    text = replace(text, '  auto payload = recv.cursor2;',
                   '  if (len && len - 1 > UINT32_MAX - dst) recv.error = true;\n  auto payload = recv.cursor2;')
    text = replace(text, '      if (addr < 0 || size <= 0 ||',
                   '      if (recv.cursor2 != recv.buf + recv.size) recv.error = true;\n      if (addr < 0 || size <= 0 ||')
    text = replace(text, '      flush_flash_cache();\n      send.set_packet("OK");',
                   '      if (recv.cursor2 != recv.buf + recv.size) recv.error = true;\n      if (!recv.error) flush_flash_cache();\n      send.set_packet(recv.error ? "E01" : "OK");')
    # Do not silently substitute software flash patches for hardware breakpoints.
    for name in ['z1', 'Z1']:
        text = replace_function(text, 'void GDBServer::handle_' + name + '()', '''  recv.cursor2 = recv.buf + recv.size;
  send.set_packet("");
  next_state = SEND_PREFIX;''')
    for name, operation in [('z0', 'clear'), ('Z0', 'set')]:
        text = replace_function(text, 'void GDBServer::handle_' + name + '()', f'''  recv.take("{name},");
  uint32_t addr = recv.take_hex();
  recv.take(',');
  uint32_t kind = recv.take_hex();
  if (recv.cursor2 != recv.buf + recv.size || (kind != 2 && kind != 4)) recv.error = true;
  int result = recv.error ? -1 : soft->{operation}_breakpoint(addr, kind);
  send.set_packet(result < 0 ? "E01" : "OK");
  next_state = SEND_PREFIX;''')
    # An invalid hex checksum digit must never alias a valid checksum byte.
    old = '        expected_checksum = (expected_checksum << 4) | from_hex(byte_in);'
    if text.count(old) != 2:
        raise ValueError('Pinned checksum parser changed')
    text = text.replace(old, '        int digit = from_hex(byte_in);\n        if (digit < 0) recv.error = true;\n' + old)
    text = replace(text, "        else if (byte_in == '}') {", '''        else if (byte_in == '$') {
          recv.clear();
          checksum = 0;
        }
        else if (byte_in == '}') {''')
    path.write_text(text)

    path = output / 'SoftBreak.h'
    path.write_text(replace(path.read_text(), '  uint32_t* breakpoints;',
                            '  uint32_t* breakpoints;\n  uint8_t breakpoint_sizes[32] = {};'))
    path = output / 'SoftBreak.cpp'
    text = path.read_text()
    text = replace(text, '  printf("  breakpoint_count %d\\n");',
                   '  printf("  breakpoint_count %d\\n", breakpoint_count);')
    # Keep both breakpoint pages coherent, including 32-bit instructions at +62.
    text = replace_function(text, 'int SoftBreak::set_breakpoint(uint32_t addr, int size)', '''  if (!halted || (size != 2 && size != 4) || (addr & 1) ||
      addr > static_cast<uint32_t>(flash->get_flash_size() - size)) return -1;
  int index = -1;
  for (int i = 0; i < breakpoint_max; ++i) {
    if (breakpoints[i] == BP_EMPTY) {
      if (index < 0) index = i;
      continue;
    }
    if (breakpoints[i] == addr && breakpoint_sizes[i] == size) return i;
    if (addr < breakpoints[i] + breakpoint_sizes[i] && breakpoints[i] < addr + size) return -1;
  }
  if (index < 0) return -1;
  for (unsigned page = addr / page_size; page <= (addr + size - 1) / page_size; ++page) {
    unsigned base = page * page_size;
    if (!break_map[page]) {
      rvd->get_block_aligned(base, flash_clean + base, page_size);
      memcpy(flash_dirty + base, flash_clean + base, page_size);
    }
    ++break_map[page];
    dirty_map[page] = 1;
  }
  const uint8_t trap2[] = {0x02, 0x90};
  const uint8_t trap4[] = {0x73, 0x00, 0x10, 0x00};
  memcpy(flash_dirty + addr, size == 2 ? trap2 : trap4, size);
  breakpoints[index] = addr;
  breakpoint_sizes[index] = size;
  ++breakpoint_count;
  return index;''')
    text = replace_function(text, 'int SoftBreak::clear_breakpoint(uint32_t addr, int size)', '''  if (!halted || (size != 2 && size != 4) || (addr & 1) ||
      addr > static_cast<uint32_t>(flash->get_flash_size() - size)) return -1;
  for (int i = 0; i < breakpoint_max; ++i) {
    if (breakpoints[i] != addr) continue;
    if (breakpoint_sizes[i] != size) return -1;
    memcpy(flash_dirty + addr, flash_clean + addr, size);
    for (unsigned page = addr / page_size; page <= (addr + size - 1) / page_size; ++page) {
      --break_map[page];
      dirty_map[page] = 1;
    }
    breakpoints[i] = BP_EMPTY;
    breakpoint_sizes[i] = 0;
    --breakpoint_count;
    return i;
  }
  return 0; // Duplicate removals are idempotent. ''')
    text = replace_function(text, 'void SoftBreak::clear_all_breakpoints()', '''  halt();
  unpatch_flash();
  for (int i = 0; i < breakpoint_max; ++i) {
    breakpoints[i] = BP_EMPTY;
    breakpoint_sizes[i] = 0;
  }
  breakpoint_count = 0;
  memset(break_map, 0, flash->get_page_count());
  memset(flash_map, 0, flash->get_page_count());
  memset(dirty_map, 0, flash->get_page_count());''')
    text = replace(text, 'void SoftBreak::reset()      { rvd->reset(); }', '''void SoftBreak::reset() {
  halt();
  rvd->reset();
  halted = true;
}''')
    path.write_text(text)


if __name__ == '__main__':
    prepare(Path(sys.argv[1]), Path(sys.argv[2]))
