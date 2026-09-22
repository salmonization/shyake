export function isValidUsername(username: string): boolean {
  const regex = /^(?=.*[a-zA-Z])[a-zA-Z0-9_]{4,16}$/;
  return regex.test(username);
}

export function isReservedUsername(
  username: string,
  reservedList: string,
): boolean {
  if (!reservedList) return false;
  const reserved = reservedList.split(',').map(s => s.trim().toLowerCase());
  return reserved.includes(username.toLowerCase());
}

// Basic Hashcash verifier (v1)
// Format: 1:bits:date:resource:ext:rand:counter
export async function verifyPoW(
  hashcash: string,
  requiredBits: number = 20,
): Promise<boolean> {
  try {
    const parts = hashcash.split(':');
    if (parts.length !== 7) return false;

    const claimedBits = parseInt(parts[1], 10);
    if (claimedBits < requiredBits) return false;

    const encoder = new TextEncoder();
    const data = encoder.encode(hashcash);
    const hashBuffer = await crypto.subtle.digest('SHA-1', data);
    const hashArray = Array.from(new Uint8Array(hashBuffer));

    let binaryStr = '';
    for (let i = 0; i < Math.ceil(requiredBits / 8) + 1; i++) {
      if (hashArray[i] !== undefined) {
        binaryStr += hashArray[i].toString(2).padStart(8, '0');
      }
    }

    return binaryStr.startsWith('0'.repeat(requiredBits));
  } catch (e) {
    return false;
  }
}

// Address normalization
//
// A shyake address is a bare local username, "user@domain", or (for
// blocklist targets) a bare domain. Usernames match
// ^(?=.*[a-zA-Z])[a-zA-Z0-9_]{4,16}$ and so can contain neither "@"
// nor ".", which is what makes the two bare forms distinguishable.
//
// Normalizing yields the form the database stores: local users bare,
// remote users "user@domain" with the domain lowercased.
export function normalizeAddress(addr: string, instanceDomain: string): string {
  const at = addr.indexOf('@');
  if (at < 0) {
    // bare domain (dotted) or bare local username
    return addr.includes('.') ? addr.toLowerCase() : addr;
  }
  const local = addr.slice(0, at);
  const domain = addr.slice(at + 1).toLowerCase();
  return domain === instanceDomain.toLowerCase() ? local : `${local}@${domain}`;
}

// Domain an address belongs to; bare names belong to this instance.
export function addressDomain(addr: string, instanceDomain: string): string {
  const at = addr.indexOf('@');
  if (at < 0) {
    return addr.includes('.')
      ? addr.toLowerCase()
      : instanceDomain.toLowerCase();
  }
  return addr.slice(at + 1).toLowerCase();
}
