namespace src {

    const char *dot =
R"(
void dot(TYPE * A, TYPE * B, TYPE * C,
         int M, int N, int K,
         int lda __multipleof(8),
         int ldb __multipleof(8),
         int ldc) {
  // prologue
  int ridx = get_program_id(0);
  int ridy = get_program_id(1);
  int rxa[TM] = ridx * TM + 0 ... TM;
  int ryb[TN] = ridy * TN + 0 ... TN;
  int rka[TK] = 0 ... TK;
  int rkb[TK] = 0 ... TK;
  float c[TM, TN] = 0;
  // pointers to operands
  TYPE* pa[SHAPE_A] = A + rka[BROADCAST_AK] * STRIDE_AK + rxa[BROADCAST_AM] * STRIDE_AM;
  TYPE* pb[SHAPE_B] = B + rkb[BROADCAST_BK] * STRIDE_BK + ryb[BROADCAST_BN] * STRIDE_BN;
  // prefetches operands
  TYPE a[SHAPE_A] = *pa;
  TYPE b[SHAPE_B] = *pb;
  // reduction loop
  for(int k = K; k > 0; k-= TK){
    c += USEA @ USEB;
    pa = pa + TK * STRIDE_AK;
    pb = pb + TK * STRIDE_BK;
    a = *pa;
    b = *pb;
  }
  // epilogue
  int rxc[TM] =  ridx * TM + 0 ... TM;
  int ryc[TN] =  ridy * TN + 0 ... TN;
  TYPE* pc[TM, TN] = C + rxc[:, newaxis] + ryc[newaxis, :] * ldc;
  *pc = c;
}
)";

}
