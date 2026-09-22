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

// Hashcash v1 verifier (SPEC 3.5)
// Format: 1:bits:yymmdd:resource:ext:rand:counter
//
// The token must name the acting user as its resource and carry a date
// within one day of today (UTC). Without that binding, one minted token
// pays for any request by anyone, forever. A federated sender's resource
// is "user@host:port" when the instance runs on a port, so the resource
// can hold colons: the three trailing fields are fixed, so cut from both
// ends.
export async function verifyPoW(
  hashcash: string,
  resource: string,
  requiredBits: number = 20,
): Promise<boolean> {
  try {
    if (hashcash.length > 256) return false;
    const parts = hashcash.split(':');
    const n = parts.length;
    if (n < 7 || parts[0] !== '1') return false;

    const claimedBits = parseInt(parts[1], 10);
    if (!(claimedBits >= requiredBits)) return false;
    if (parts.slice(3, n - 3).join(':') !== resource) return false;
    if (!freshDate(parts[2])) return false;

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

// yymmdd within one day of today (UTC), for clock skew around midnight
function freshDate(yymmdd: string): boolean {
  if (!/^[0-9]{6}$/.test(yymmdd)) return false;
  const day = Date.UTC(
    2000 + parseInt(yymmdd.slice(0, 2), 10),
    parseInt(yymmdd.slice(2, 4), 10) - 1,
    parseInt(yymmdd.slice(4, 6), 10),
  );
  const today = Math.floor(Date.now() / 86400000) * 86400000;
  return Math.abs(day - today) <= 86400000;
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
