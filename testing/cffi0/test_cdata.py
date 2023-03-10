from cffi import FFI

class FakeBackend(object):

    def nonstandard_integer_types(self):
        return {}

    def sizeof(self, name):
        return 1

    def load_library(self, path):
        return "fake library"

    def new_primitive_type(self, name):
        return FakeType("primitive " + name)

    def new_void_type(self):
        return FakeType("void")
    def new_pointer_type(self, x):
        return FakeType('ptr-to-%r' % (x,))
    def new_array_type(self, x, y):
        return FakeType('array-from-%r-len-%r' % (x, y))
    def cast(self, x, y):
        return 'casted!'
    def _get_types(self):
        return "CData", "CType"

    buffer = "buffer type"


class FakeType(object):
    def __init__(self, cdecl):
        self.cdecl = cdecl


def test_typeof():
    ffi = FFI(backend=FakeBackend())
    clong = ffi.typeof("signed long int")
    assert isinstance(clong, FakeType)
    assert clong.cdecl == 'primitive long'
