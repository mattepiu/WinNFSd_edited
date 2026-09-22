#include "SocketStream.h"
#include <string.h>

#define MAXDATA (1024 * 1024)

CSocketStream::CSocketStream()
{
	m_pInBuffer = new unsigned char[MAXDATA];
	m_pOutBuffer = new unsigned char[MAXDATA];
	m_nInBufferIndex = m_nInBufferSize = 0;
	m_nOutBufferIndex = m_nOutBufferSize = 0;
}

CSocketStream::~CSocketStream()
{
	delete[] m_pInBuffer;
	delete[] m_pOutBuffer;
}

unsigned char *CSocketStream::GetInput(void)
{
	return m_pInBuffer;
}

void CSocketStream::SetInputSize(unsigned int nSize)
{
	m_nInBufferIndex = 0;  //seek to the beginning of the input buffer
	m_nInBufferSize = nSize;
}

unsigned char *CSocketStream::GetOutput(void)
{
	return m_pOutBuffer;  //output buffer
}

unsigned int CSocketStream::GetOutputSize(void)
{
	return m_nOutBufferSize;  //number of bytes of data in the output buffer
}

unsigned int CSocketStream::GetBufferSize(void)
{
	return MAXDATA;  //size of input/output buffer
}

unsigned int CSocketStream::Read(void *pData, unsigned int nSize)
{
	if (nSize > m_nInBufferSize - m_nInBufferIndex)  //over the number of bytes of data in the input buffer
		nSize = m_nInBufferSize - m_nInBufferIndex;
	memcpy(pData, m_pInBuffer + m_nInBufferIndex, nSize);
	m_nInBufferIndex += nSize;
	return nSize;
}

unsigned int CSocketStream::Read(unsigned long *pnValue)
{
	unsigned int i, n;
	unsigned char *p, pBuffer[sizeof(unsigned long)];

	p = (unsigned char *)pnValue;
	n = Read(pBuffer, sizeof(unsigned long));
	for (i = 0; i < n; i++)  //reverse byte order
		p[sizeof(unsigned long) - 1 - i] = pBuffer[i];
	return n;
}

unsigned int CSocketStream::Read8(unsigned __int64 *pnValue)
{
	unsigned int i, n;
	unsigned char *p, pBuffer[sizeof(unsigned __int64)];

	p = (unsigned char *)pnValue;
	n = Read(pBuffer, sizeof(unsigned __int64));
	for (i = 0; i < n; i++)  //reverse byte order
		p[sizeof(unsigned __int64) - 1 - i] = pBuffer[i];
	return n;
}

unsigned int CSocketStream::Skip(unsigned int nSize)
{
	if (nSize > m_nInBufferSize - m_nInBufferIndex)  //over the number of bytes of data in the input buffer
		nSize = m_nInBufferSize - m_nInBufferIndex;
	m_nInBufferIndex += nSize;
	return nSize;
}

unsigned int CSocketStream::GetSize(void)
{
	return m_nInBufferSize - m_nInBufferIndex;  //number of bytes of rest data in the input buffer
}

unsigned char *CSocketStream::GetInputTail(void)
{
	return m_pInBuffer + m_nInBufferSize;  //where the next recv() should append
}

unsigned int CSocketStream::GetInputSpace(void)
{
	return MAXDATA - m_nInBufferSize;  //bytes still free in the input buffer
}

void CSocketStream::AddInputSize(unsigned int nSize)
{
	m_nInBufferSize += nSize;  //bytes appended at the tail
}

void CSocketStream::CompactInput(void)
{
	if (m_nInBufferIndex > 0)  //drop already-consumed bytes
	{
		if (m_nInBufferIndex < m_nInBufferSize)
			memmove(m_pInBuffer, m_pInBuffer + m_nInBufferIndex, m_nInBufferSize - m_nInBufferIndex);
		m_nInBufferSize -= m_nInBufferIndex;
		m_nInBufferIndex = 0;
	}
}

bool CSocketStream::HasCompleteRecord(void)
{
	unsigned int avail, len;
	unsigned char *p;

	avail = m_nInBufferSize - m_nInBufferIndex;
	if (avail < 4)
		return false;
	p = m_pInBuffer + m_nInBufferIndex;
	/* RPC record marking: big-endian 32-bit header, high bit = last fragment,
	 * low 31 bits = number of bytes that follow the header. */
	len = ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | (unsigned int)p[3];
	len &= 0x7FFFFFFF;
	return avail >= 4 + len;
}

void CSocketStream::Write(void *pData, unsigned int nSize)
{
	if (m_nOutBufferIndex + nSize > MAXDATA)  //over the size of output buffer
		nSize = MAXDATA - m_nOutBufferIndex;
	memcpy(m_pOutBuffer + m_nOutBufferIndex, pData, nSize);
	m_nOutBufferIndex += nSize;
	if (m_nOutBufferIndex > m_nOutBufferSize)
		m_nOutBufferSize = m_nOutBufferIndex;
}

void CSocketStream::Write(unsigned long nValue)
{
	int i;
	unsigned char *p, pBuffer[sizeof(unsigned long)];

	p = (unsigned char *)&nValue;
	for (i = sizeof(unsigned long) - 1; i >= 0; i--)  //reverse byte order
		pBuffer[i] = p[sizeof(unsigned long) - 1 - i];
	Write(pBuffer, sizeof(unsigned long));
}

void CSocketStream::Write8(unsigned __int64 nValue)
{
	int i;
	unsigned char *p, pBuffer[sizeof(unsigned __int64)];

	p = (unsigned char *)&nValue;
	for (i = sizeof(unsigned __int64) - 1; i >= 0; i--)  //reverse byte order
		pBuffer[i] = p[sizeof(unsigned __int64) - 1 - i];
	Write(pBuffer, sizeof(unsigned __int64));
}

void CSocketStream::Seek(int nOffset, int nFrom)
{
	if (nFrom == SEEK_SET)
		m_nOutBufferIndex = nOffset;
	else if (nFrom == SEEK_CUR)
		m_nOutBufferIndex += nOffset;
	else if (nFrom == SEEK_END)
		m_nOutBufferIndex = m_nOutBufferSize + nOffset;
}

int CSocketStream::GetPosition(void)
{
	return m_nOutBufferIndex;
}

void CSocketStream::Reset(void)
{
	m_nOutBufferIndex = m_nOutBufferSize = 0;  //clear output buffer
}
