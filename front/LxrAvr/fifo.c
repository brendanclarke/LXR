/*
 * fifo.c
 *
 * Created: 26.04.2012 16:57:26
 *  Author: Julian
 */ 
#include "fifo.h"
//----------------------------------------------------------------------------
void fifo_clear(FifoBuffer* fifo)
{
   fifo->read=fifo->write;
}

//----------------------------------------------------------------------------
void fifo_init(FifoBuffer* fifo)
{
   memset((void*)fifo->data,0,BUFFER_SIZE);
   fifo->read = 0;
   fifo->write = 0;
}	
//----------------------------------------------------------------------------
uint8_t fifo_bufferIn(FifoBuffer* fifo, uint8_t byte)
{
   uint8_t next = (uint8_t)((fifo->write + 1) & BUFFER_MASK);
   if (fifo->read == next)
      return 0;
   fifo->data[fifo->write] = byte;
   fifo->write = next;
   return 1;
}
//----------------------------------------------------------------------------
uint8_t fifo_BufferOut(FifoBuffer* fifo, uint8_t *pByte)
{
   if (fifo->read == fifo->write)
      return 0;
   *pByte = fifo->data[fifo->read];
   fifo->read = (uint8_t)((fifo->read+1) & BUFFER_MASK);
   return 1;
}
//----------------------------------------------------------------------------
uint8_t fifo_count(FifoBuffer* fifo)
{
   return (uint8_t)((fifo->write - fifo->read) & BUFFER_MASK);
}
//----------------------------------------------------------------------------
uint8_t fifo_free(FifoBuffer* fifo)
{
   return (uint8_t)(BUFFER_MASK - fifo_count(fifo));
}
//----------------------------------------------------------------------------
uint8_t fifo_isEmpty(FifoBuffer* fifo)
{
   return (fifo->read == fifo->write);
}
//----------------------------------------------------------------------------
