# Host fixture for the topo-init scaffold smoke: a free function plus a
# class without __init__ (declared constructors are not matchable yet).


def add(a, b):
    return a + b


class Greeter:
    def message(self):
        return "topo"
