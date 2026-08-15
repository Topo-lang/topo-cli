// Host fixture for the topo-init scaffold smoke: a free function plus a
// class without a constructor (declared constructors are not matchable
// yet).

export function add(a: number, b: number): number {
  return a + b;
}

export class Greeter {
  message(): string {
    return "topo";
  }
}
