#pragma once

#include <FFGLSDK.h>

namespace photofinish
{
/**
    An off-screen buffer for one stage of the chain.

    Copied from tinsel, where it was written, and kept verbatim apart from the
    header comment: the SDK defect it works around is the same one everywhere,
    and a second implementation of it is a second thing to get wrong.

    Three things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** Ensure() is called every frame and
    is a no-op in the overwhelming majority of them. The ring reallocates when
    the picture changes size, when Axis is flipped, and whenever Sweep Length
    moves -- which an operator drags -- so this path has to be cheap.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` --
    so the colour texture is leaked on every release (SDK b1afaf9,
    `FFGLFBO.cpp`). `Destroy()` deletes it first. It matters here rather than
    being pedantry: the ring is a full picture-sized RGBA8 buffer and it is
    reallocated every time the operator moves Sweep Length.

    **It owns its filtering**, and this plugin wants two different answers:

    - the two frame copies are read *between* texels -- a tilted or wide slit
      lands at fractional source coordinates -- so they are `GL_LINEAR`. That
      is sampling SPACE, which the slit is entitled to do;
    - the ring is `GL_NEAREST`, always. A column of the ring is one instant,
      and blending two of them together would be inventing a moment that was
      never sampled. See AGENTS.md: it is also what makes `--static` bitwise
      at a magnifying Sweep Length instead of bitwise only at 1:1.

    `Sampling::Mipmapped` is carried over unused. Nothing here reduces a
    picture, and a mip chain on the ring would be a chain of averaged instants.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	enum class Sampling
	{
		Nearest,  ///< for data read texel-for-texel. No filtering, no mip chain.
		Linear,   ///< for pictures read between texels. Bilinear, no mip chain.
		Mipmapped ///< for pictures that also get reduced. Trilinear + GenerateMipmaps().
	};

	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a buffer whose
	/// contents are undefined is not "a bit of noise on the first frame", it is
	/// whatever texture memory the driver handed back -- and for the edge
	/// history buffers, which feed back into themselves, it is noise that never
	/// washes out.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Sampling sampling );

	/// Rebuild the mip chain from level 0. Call after rendering into a
	/// Sampling::Mipmapped buffer and before anything samples it; a stale chain
	/// does not look like an error, it looks like the wrong footage.
	void GenerateMipmaps();

	/// Highest mip level this buffer has, i.e. the 1x1 one. The centroid pass
	/// needs it as a uniform: `textureQueryLevels` is GLSL 4.30 and these
	/// shaders are 4.10.
	float MaxMipLevel() const;

	/// Clear to transparent black. The edge history needs this when the effect
	/// is re-enabled or the source changes size, so the first stabilised frame
	/// blends against nothing rather than against the last clip.
	void Clear();

	/// The colour texture, for binding as an input to a later pass.
	///
	/// The SDK keeps `colorTextureID` protected and offers only
	/// `GetTextureInfo()`, which builds and returns an `FFGLTextureStruct` --
	/// six fields assembled to reach one of them, at every bind of every pass
	/// of every frame. A subclass can just say which texture it is.
	GLuint TextureID() const
	{
		return colorTextureID;
	}

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

private:
	Sampling sampling = Sampling::Nearest;
};

} // namespace photofinish
