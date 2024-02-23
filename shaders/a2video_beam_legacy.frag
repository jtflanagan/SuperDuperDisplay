#ifdef GL_ES
#define COMPAT_PRECISION mediump
precision mediump float;
precision highp usampler2D;
precision highp int;
#else
#define COMPAT_PRECISION
layout(pixel_center_integer) in vec4 gl_FragCoord;
#endif

/*
Apple 2 video beam shader for legacy modes (not SHR).

This shader expects as input a VRAMTEX texture that has the following features:
- Type GL_RGB8UI, which is 3 bytes for each texel
- Color R is the MAIN memory byte
- Color G is the AUX memory byte
- Color B is 8 bits of state, including the graphics mode and soft switches
- 40 pixels wide, where MAIN and AUX are interleaved, starting with AUX
- 192 lines high, which is the Apple 2 scanlines

The shader goes through the following phases:
- The fragment determines which VRAMTEX texel it's part of, including the x offset
	to the start of the texel (there is no y offset, each byte is on one scanline).
- It grabs the texel and determines the video mode to use
- It runs the video mode code on that byte and chooses the correct fragment

*/

// Global uniforms
uniform int ticks;						// ms since start
uniform usampler2D VRAMTEX;				// Video RAM texture
uniform sampler2D a2ModesTextures[5];	// 2 font textures + lgr, hgr, dhgr
uniform vec4 colorTint;					// text color tint (extra from 2gs)

// mode -> texture
// modes are: TEXT, DTEXT, LGR, DLGR, HGR, DHGR
// For 40 col text, texture 0 is normal and 1 is alternate
// For 80 col text, texture 2 is normal and 3 is alternate
const int modeToTexture[6] = int[6](0, 2, 4, 4, 5, 6);

in vec2 vFragPos;       // The fragment position in pixels
out vec4 fragColor;

void main()
{
	// first determine which VRAMTEX texel this fragment is part of, including
	// the x and y offsets from the origin
	// REMINDER: we're working on dots, with 560 dots per line. And lines are doubled
	uvec2 uFragPos = uvec2(vFragPos);
	uvec3 targetTexel =  texelFetch(VRAMTEX, ivec2(uFragPos.x / 14u, uFragPos.y / 2u), 0).rbg;
	uvec2 fragOffset = uvec2(uFragPos.x % 14u, uFragPos.y % 16);	// TODO: IS IT uvec2 or vec2 ???
	// The fragOffsets are:
	// x is 0-14
	// y is 0-16

	// Extract the lower 3 bits to determine which mode to use
	uint a2mode = targetTexel.b & 7u;	// 7 = 0b111 to mask lower 3 bits
    uint textureIndex = modeToTexture[a2mode];	// this is the texture to use
	
	switch (a2mode) {
		case 0u:	// TEXT
		case 1u:	// DTEXT
		{
			// Get the character value
			// In DTEXT mode, the first 7 dots are AUX, last 7 are MAIN.
			// In TEXT mode, all 14 dots are from MAIN
			uint charVal = (targetTexel.g * (fragOffset.x / 7u) + targetTexel.r * (1u - (fragOffset.x / 7u))) * a2mode
							+ targetTexel.r * (1 - a2mode);
			float vCharVal = float(charVal);
			
			// if ALTCHARSET (bit 4), use the alt texture
			uint isAlt = ((targetTexel.b >> 4) & 1u);
			textureIndex += 1u * isAlt;
			
			// Determine from char which font glyph to use
			// and if we need to flash
			// Determine if it's inverse when the char is below 0x40
			// And then if the char is below 0x80 and not inverse, it's flashing,
			// but only if it's the regular charset
			float a_inverse = 1.0 - step(float(0x40), vCharVal);
			float a_flash = (1.0 - step(float(0x80), vCharVal)) * (1.0 - a_inverse) * (1.0 - float(isAlt));
			
			// what's our character's starting origin in the character map?
			// each glyph is 14x16 for TEXT and 7x16 for DTEXT
			uvec2 charOrigin = uvec2(charVal & 0xFu, charVal >> 4) * uvec2(7 * (2 - a2mode), 16);
			
			// Now get the texture color
			// When getting from the texture color, in DTEXT if we didn't have a dedicated set of 7x16 glyphs
			// we would have multiplied the x value by 2 to take 1/2 of each column in 80 col mode.
			// But we have a dedicated set of 7x16 font textures so don't need to
			ivec2 textureSize2d = textureSize(a2ModesTextures[textureIndex],0);
			vec4 tex = texture(a2ModesTextures[textureIndex], (vec2(charOrigin) + fragOffset) / vec2(textureSize2d)) * colorTint;
			
			float isFlashing =  a_flash * float((ticks / 310) % 2);    // Flash every 310ms
																	   // get the color of flashing or the one above
			fragColor = ((1.f - tex) * isFlashing) + (tex * (1.f - isFlashing));
			return;
			break;
		}
		case 2u:	// LGR
		case 3u:	// DLGR
		{
			// Get the color value
			// An LGR byte is split in 2. There's a 4-bit color in the low bits
			// at the top of the 14x16 dot square, and another 4-bit color in
			// the high bits at the bottom of the 14x16 dot square.
			// In DLGR mode, the first 7 dots are AUX, last 7 are MAIN.
			// In LGR mode, all 14 dots are from MAIN

			// Get the byte value depending on MAIN or AUX
			uint byteVal = (targetTexel.g * (fragOffset.x / 7u) + targetTexel.r * (1u - (fragOffset.x / 7u))) * a2mode
				+ targetTexel.r * (1 - a2mode);
			// get the color depending on vertical position
			uvec2 byteOrigin;
			if (fragOffset.y < 8)
			{
				// This is a top pixel, it uses the color of the 4 low bits
				byteOrigin = uvec2(0u, (byteVal & 0xFu) * 16u);
			} else {
				// It's a bottom pixel, it uses the color of the 4 high bits
				byteOrigin = uvec2(0u, (byteVal >> 4) * 16u);
			}
			ivec2 textureSize2d = textureSize(a2ModesTextures[textureIndex],0);
			// similarly to the TEXT modes, if we're in DLGR (a2mode - 2u), get every other column
			fragColor = texture(a2ModesTextures[textureIndex],
								(vec2(byteOrigin) + (fragOffset * uvec2(1u + (a2mode - 2u), 1u))) / vec2(textureSize2d));
			break;
		}
		case 4u:	// HGR
		{
/*
For each pixel, determine which memory byte it is part of,
 and save the x offset from the origin of the byte.

 To get a pixel in the HGR texture, the procedure is as follows:
 Take the byte that the pixel is in.
 Even bytes use even columns, odd bytes use odd columns.
 Also calculate the high bit and last 2 bits from the previous byte
 (i.e. the 3 most significant bits), and the first 2 bits from the
 next byte (i.e. the 3 least significant bits).

 // Lookup Table:
 // y (0-255) * 32 columns of 32 pixels
 // . each column is: high-bit (prev byte) & 2 pixels from previous byte & 2 pixels from next byte
 // . each 32-pixel unit is 2 * 16-pixel sub-units: 16 pixels for even video byte & 16 pixels for odd video byte
 //   . where 16 pixels represent the 7 Apple pixels, expanded to 14 pixels (and the last 2 are discarded)
 //		currHighBit=0: {14 pixels + 2 pad} * 2
 //		currHighBit=1: {1 pixel + 14 pixels + 1 pad} * 2

 high-bit & 2-bits from previous byte, 2-bits from next byte = 2^5 = 32 total permutations
 32 permutations, each with 2 bytes, each 8 bits but doubled: 32 * 2 * 8 * 2 = 1024 pixels wide
 So the col offset is ((prevbyte & 0xE0) >> 3) | (nextbyte & 0x03). But since each column is
 32 pixels, the actual col pixel offset should be *32, which results in:
 ((prevbyte & 0xE0) << 2) | ((nextbyte & 0x03) << 5)
 Then we also need to see which of the 2 subcolumns we will use, depending if it's an even or odd byte:
 ((prevbyte & 0xE0) << 2) | ((nextbyte & 0x03) << 5) + (tileColRow.x & 1) * 16
 The row pixel value is simply the memory byte value of our pixel
 */
 
			// The byte value is just targetTexel.r
			// Grab the other byte values that matter
			uint byteValPrev = 0u;
			uint byteValNext = 0u;
			int xCol = int(uFragPos.x) / 14;
			if (xCol > 0)	// Not at start of row, byteValPrev is valid
			{
				byteValPrev = texelFetch(VRAMTEX, ivec2(xCol - 1, uFragPos.y / 2u), 0).r;
			}
			if (xCol < 39)	// Not at end of row, byteValNext is valid
			{
				byteValNext = texelFetch(VRAMTEX, ivec2(xCol + 1, uFragPos.y / 2u), 0).r;
			}

			// calculate the column offset in the color texture
			int texXOffset = (int((byteValPrev & 0xE0u) << 2) | int((byteValNext & 0x03u) << 5)) + (xCol & 1) * 16;

			// Now get the texture color. We know the X offset as well as the fragment's offset on top of that.
			// The y value is just the byte's value
			ivec2 textureSize2d = textureSize(a2ModesTextures[textureIndex],0);
			fragColor = texture(a2ModesTextures[textureIndex], vec2(texXOffset + int(fragOffset.x), targetTexel.r) / vec2(textureSize2d));
			return;
			break;
		}
		case 5u:	// DHGR
		{
/*
 For each pixel, determine which memory byte it is part of,
 and save the x offset from the origin of the byte.

 There are 256 columns of 10 pixels in the DHGR texture. Acquiring the right data is a lot more complicated.
 It involves taking 20 bits out of 4 memory bytes, then shifting and grabbing different bytes for x and y
 in the texture. See UpdateDHiResCell() in RGBMonitor.cpp of the AppleWin codebase. Take 7 bits each of
 the 2 middle bytes and 3 bits each of the 2 end bytes for a total of 20 bits.
 */
			// TODO: check for DHGRMONO

			// In DHGR, as in all double modes, the even bytes are from AUX, odd bytes from MAIN
			// We already have in targetTexel both MAIN and AUX bytes (R and G respectively)
			// We need a previous MAIN byte and a subsequent AUX byte to calculate the colors
			uint byteVal1 = 0u;				// MAIN
			uint byteVal2 = targetTexel.g;	// AUX
			uint byteVal3 = targetTexel.r;	// MAIN
			uint byteVal4 = 0u;				// AUX
			int xCol = int(uFragPos.x) / 14;
			if (xCol > 0)	// Not at start of row, byteVal1 is valid
			{
				byteVal1 = texelFetch(VRAMTEX, ivec2(xCol - 1, uFragPos.y / 2u), 0).r;
			}
			if (xCol < 39)	// Not at end of row, byteVal4 is valid
			{
				byteVal4 = texelFetch(VRAMTEX, ivec2(xCol + 1, uFragPos.y / 2u), 0).g;
			}
			// Calculate the column offset in the color texture
			int wordVal = (int(byteVal1) & 0x70) | ((int(byteVal2) & 0x7F) << 7) |
				((int(byteVal3) & 0x7F) << 14) | ((int(byteVal4) & 0x07) << 21);
			int vColor = (xCol*14 + int(fragOffset.x)) & 3;
			int vValue = (wordVal >> (4 + int(fragOffset.x) - vColor));
			int xVal = 10 * ((vValue >> 8) & 0xFF) + vColor;
			int yVal = vValue & 0xFF;
			ivec2 textureSize2d = textureSize(a2ModesTextures[textureIndex],0);
			fragColor = texture(a2ModesTextures[textureIndex], (vec2(0.5, 0.5) + vec2(xVal, yVal)) / vec2(textureSize2d));
			return;
			break;
		}
		default:
		{
			// should never happen! Set to pink for visibility
			fragColor = vec4(1.0f, 0.f, 0.5f, 1.f);
			return;
			break;
		}
	}
	// Shouldn't happen either
	fragColor = vec4(0.f, 1.0f, 0.5f, 1.f);
}
