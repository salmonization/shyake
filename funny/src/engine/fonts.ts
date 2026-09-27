// Load the film's typefaces before the first frame is drawn

const FACES: [string, string, FontFaceDescriptors][] = [
    ['EB Garamond', 'EBGaramond.woff2', { weight: '400 800' }],
    ['EB Garamond', 'EBGaramond-Italic.woff2', { weight: '400 800', style: 'italic' }],
    ['FreeSans', 'FreeSans.woff2', { weight: '400' }],
    ['FreeSans', 'FreeSansBold.woff2', { weight: '700' }],
    ['FreeMono', 'FreeMono.woff2', { weight: '400' }],
    ['FreeMono', 'FreeMonoBold.woff2', { weight: '700' }],
];

export async function loadFonts(base: string): Promise<void> {
    await Promise.all(
        FACES.map(async ([family, file, desc]) => {
            const face = new FontFace(family, `url(${base}fonts/${file})`, desc);
            await face.load();
            document.fonts.add(face);
        }),
    );
}

export async function loadImage(url: string): Promise<HTMLImageElement> {
    const img = new Image();
    img.src = url;
    await img.decode();
    return img;
}
