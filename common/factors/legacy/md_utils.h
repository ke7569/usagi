//
// Created by Administrator on 25-10-12.
//

#ifndef MD_UTILS_H
#define MD_UTILS_H



class md_utils {

};

#define OD_BUY(od) (od->Direction == '1')
#define OD_SELL(od) (od->Direction == '2')
#define SH_OD_INSERT(od) (od->OrdType == 'A')
#define SH_OD_CXL(od) (od->OrdType == 'D')


#endif //MD_UTILS_H
