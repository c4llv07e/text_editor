#!/usr/bin/env python3
void_type = gdb.lookup_type("void")
class StringPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        size = int(self.val['size'])
        text = self.val['text']
        if text == 0:
            return f"[{size}]<null>"
        if size == 0:
            return f"[{size}]<{text.cast(gdb.Type.pointer(void_type))}>"
        try:
            content = text.string(length=size)
            return f"[{size}]\"{content}\""
        except:
            return f"[{size}]<{text}>"

def pretty_print_sized_string(val):
    if str(val.type) == 'String':
        return StringPrinter(val)
    return None

gdb.pretty_printers = []
gdb.pretty_printers.append(pretty_print_sized_string)
