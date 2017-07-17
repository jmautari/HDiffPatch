//diff.cpp
//
/*
 The MIT License (MIT)
 Copyright (c) 2012-2017 HouSisong

 Permission is hereby granted, free of charge, to any person
 obtaining a copy of this software and associated documentation
 files (the "Software"), to deal in the Software without
 restriction, including without limitation the rights to use,
 copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following
 conditions:

 The above copyright notice and this permission notice shall be
 included in all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 OTHER DEALINGS IN THE SOFTWARE.
*/

#include "diff.h"
#include <assert.h>
#include <vector>
#include "private_diff/suffix_string.h"
#include "private_diff/bytes_rle.h"
#include "private_diff/pack_uint.h"
#include "../HPatch/patch.h"


namespace{
    
    typedef unsigned char TByte;
    typedef size_t        TUInt;
    typedef ptrdiff_t     TInt;
    static const int kMinTrustMatchLength=1024*8;  //(贪婪)选定该覆盖线(优化一些速度).
    static const int kUnLinkLength=6;              //搜索时不能合并的代价.
    static const int kMaxLinkSpaceLength=(1<<9)-1; //跨覆盖线合并时,允许合并的最远距离.
    
    //覆盖线.
    struct TOldCover {
        TInt   newPos;
        TInt   oldPos;
        TInt   length;
        inline TOldCover():newPos(0),oldPos(0),length(0) { }
        inline TOldCover(TInt _newPos,TInt _oldPos,TInt _length)
            :newPos(_newPos),oldPos(_oldPos),length(_length) { }
        inline TOldCover(const TOldCover& cover)
            :newPos(cover.newPos),oldPos(cover.oldPos),length(cover.length) { }
        
        inline bool isCanLink(const TOldCover& next)const{//覆盖线是否可以连接.
            return isCollinear(next)&&(linkSpaceLength(next)<=kMaxLinkSpaceLength);
        }
        inline bool isCollinear(const TOldCover& next)const{//覆盖线是否在同一条直线上.
            return (oldPos-next.oldPos==newPos-next.newPos);
        }
        inline TInt linkSpaceLength(const TOldCover& next)const{//覆盖线间的间距.
            return next.oldPos-(oldPos+length);
        }
        inline void Link(const TOldCover& next){//共线的2覆盖线合并链接.
            assert(isCollinear(next));
            assert(oldPos<=next.oldPos);
            length = (next.oldPos-oldPos)+next.length;
        }
    };


struct TDiffData{
    const TByte*            newData;
    const TByte*            newData_end;
    const TByte*            oldData;
    const TByte*            oldData_end;
    std::vector<TOldCover>  cover;          //选出的覆盖线.
    std::vector<TByte>      newDataDiff;    //集中储存newData中没有被覆盖线覆盖住的字节数据.
    std::vector<TByte>      newDataSubDiff; //newData中的每个数值减去对应的cover线条的oldData数值和newDataDiff的数值.
};


//查找相等的字符串长度.
static TInt getEqualLength(const TByte* x,const TByte* x_end,
                           const TByte* y,const TByte* y_end){
    const TInt xLen=(TInt)(x_end-x);
    const TInt yLen=(TInt)(y_end-y);
    const TInt maxEqLen=(xLen<yLen)?xLen:yLen;
    for (TInt i=0; i<maxEqLen; ++i) {
        if (x[i]!=y[i])
            return i;
    }
    return maxEqLen;
}

//得到最长的一个匹配长度和其位置.
static bool getBestMatch(const TSuffixString& sstring,const TByte* newData,const TByte* newData_end,
                         TInt* out_pos,TInt* out_length,int kMinMatchLength){
    TInt sai=sstring.lower_bound(newData,newData_end);
    
    const TByte* src_begin=sstring.src_begin();
    const TByte* src_end=sstring.src_end();
    TInt bestLength=-1;
    TInt bestOldPos=-1;
    for (TInt i=sai-1; i<=sai; ++i) {
        if ((i<0)||(i>=(src_end-src_begin))) continue;
        TInt curOldPos=sstring.SA(i);
        TInt curLength=getEqualLength(newData,newData_end,src_begin+curOldPos,src_end);
        if (curLength>bestLength){
            bestLength=curLength;
            bestOldPos=curOldPos;
        }
    }

    *out_pos=bestOldPos;
    *out_length=bestLength;
    return (bestLength>=kMinMatchLength);
}

//粗略估算区域内当作覆盖时的可能收益.
static TInt getCoverScore(TInt newPos,TInt oldPos,TInt length,const TDiffData& diff){
    if ((oldPos<0)||(oldPos+length>(diff.oldData_end-diff.oldData)))
        return -kUnLinkLength;
    TInt eqScore=0;
    bool eq_state=true;
    for (TInt i=0; i<length; ++i) {
        if (diff.newData[newPos+i]==diff.oldData[oldPos+i]){
            ++eqScore;
            if (eq_state) //认为切换是有成本的;
                ++eqScore;
            eq_state=true;
        }else{
            eq_state=false;
        }
    }
    return (eqScore+1)>>1;
}
    
//尝试延长lastCover来完全代替matchCover;
static bool tryLinkExtend(TOldCover& lastCover,const TOldCover& matchCover,const TDiffData& diff){
    if (lastCover.length<=0) return false;
    const TInt linkSpaceLength=(matchCover.newPos-(lastCover.newPos+lastCover.length));
    if ((linkSpaceLength>kMaxLinkSpaceLength)||(matchCover.newPos==0))
        return false;
    if (lastCover.isCollinear(matchCover)){//已经共线;
        lastCover.Link(matchCover);
        return true;
    }
    TInt linkOldPos=lastCover.oldPos+lastCover.length+linkSpaceLength;
    TInt lastLinkScore=getCoverScore(matchCover.newPos,linkOldPos,matchCover.length,diff);
    if ((lastLinkScore<=0)|(lastLinkScore+kUnLinkLength<=matchCover.length))
        return false;
    TInt len=lastCover.length+linkSpaceLength+lastLinkScore*2/3;//扩展大部分,剩下的可能扩展留给extend_cover.
    len+=getEqualLength(diff.newData+lastCover.newPos+len,diff.newData_end,
                        diff.oldData+lastCover.oldPos+len,diff.oldData_end);
    while ((len>0) && (diff.newData[lastCover.newPos+len-1]
                       !=diff.oldData[lastCover.oldPos+len-1])) {
        --len;
    }
    lastCover.length=len;
    return true;
}
    
//尝试设置lastCover为matchCover所在直线的延长线,实现共线(增加合并可能等);
static void tryCollinear(TOldCover& lastCover,const TOldCover& matchCover,const TDiffData& diff){
    if (lastCover.length<=0) return;
    if (lastCover.isCollinear(matchCover)) return; //已经共线;
    TInt linkOldPos=matchCover.oldPos-(matchCover.newPos-lastCover.newPos);
    TInt lastScore=getCoverScore(lastCover.newPos,lastCover.oldPos,lastCover.length,diff);
    TInt matchLinkEqLength=getCoverScore(lastCover.newPos,linkOldPos,lastCover.length,diff);
    if (lastScore<=matchLinkEqLength)
        lastCover.oldPos=linkOldPos;
}

//寻找合适的覆盖线.
static void search_cover(TDiffData& diff,const TSuffixString& sstring,int kMinMatchLength){
    if (sstring.SASize()<=0) return;
    const TInt maxSearchNewPos=(diff.newData_end-diff.newData)-kMinMatchLength;
    TInt newPos=0;
    TOldCover lastCover(0,0,0);
    while (newPos<=maxSearchNewPos) {
        TInt matchOldPos=0;
        TInt matchEqLength=0;
        bool isMatched=getBestMatch(sstring,diff.newData+newPos,diff.newData_end,
                                    &matchOldPos,&matchEqLength,kMinMatchLength);
        if (!isMatched){
            ++newPos;//下一个需要匹配的字符串(逐位置匹配速度会比较慢).
            continue;
        }//else matched
        
        TOldCover matchCover(newPos,matchOldPos,matchEqLength);
        if (tryLinkExtend(lastCover,matchCover,diff)){//use link
            if (diff.cover.empty())
                diff.cover.push_back(lastCover);
            else
                diff.cover.back()=lastCover;
        }else{ //use match
            if (!diff.cover.empty())//尝试共线;
                tryCollinear(diff.cover.back(),matchCover,diff);
            diff.cover.push_back(matchCover);
        }
        lastCover=diff.cover.back();
        newPos=std::max(newPos+1,lastCover.newPos+lastCover.length);
    }
}

//选择合适的覆盖线,去掉不合适的.
static void select_cover(TDiffData& diff,int kMinSingleMatchLength){
    std::vector<TOldCover>&  cover=diff.cover;

    const TInt coverSize_old=(TInt)cover.size();
    TInt insertIndex=0;
    for (TInt i=0;i<coverSize_old;++i){
        if (cover[i].oldPos<0) continue;//处理已经del的.
        bool isNeedSave=cover[i].length>=kMinTrustMatchLength;//足够长.
        if (!isNeedSave){//向前合并可能.
            if ((insertIndex>0)&&(cover[insertIndex-1].isCanLink(cover[i])))
                isNeedSave=true;
        }
        if ((!isNeedSave)&&(i+1<coverSize_old)){//查询向后合并可能link
            if (cover[i].isCanLink(cover[i+1])){//右邻直接判断.
                isNeedSave=true;
            }
        }
        if (!isNeedSave){//单覆盖是否保留.
            TInt linkScore=getCoverScore(cover[i].newPos,(TInt)(diff.oldData_end-diff.oldData),
                                            cover[i].length,diff);
            isNeedSave=(cover[i].length-linkScore>=kMinSingleMatchLength);
        }

        if (isNeedSave){
            if ((insertIndex>0)&&(cover[insertIndex-1].isCanLink(cover[i]))){//link合并.
                cover[insertIndex-1].Link(cover[i]);
            }else{
                cover[insertIndex]=cover[i];
                ++insertIndex;
            }
        }
    }
    cover.resize(insertIndex);
}

    
    //得到可以扩展位置的长度.
    static TInt getCanExtendLength(TInt oldPos,TInt newPos,int inc,
                                   TInt newPos_min,TInt newPos_end,const TDiffData& diff){
        typedef size_t TFixedFloatSmooth; //定点数.
        static const TFixedFloatSmooth kFixedFloatSmooth_base=1024*8;//定点数小数点位置.
        static const TFixedFloatSmooth kExtendMinSameRatio=
                            (TFixedFloatSmooth)(0.463f*kFixedFloatSmooth_base); //0.40--0.55
        static const unsigned int kSmoothLength=4;

        TFixedFloatSmooth curBestSameRatio=0;
        TInt curBestLength=0;
        TUInt curSameCount=0;
        const TFixedFloatSmooth kLimitSameCount=(~(TFixedFloatSmooth)0)/kFixedFloatSmooth_base;
        for (TUInt length=1; (oldPos>=0)&&(oldPos<(diff.oldData_end-diff.oldData))
             &&(newPos>=newPos_min)&&(newPos<newPos_end); ++length,oldPos+=inc,newPos+=inc) {
            if (diff.oldData[oldPos]==diff.newData[newPos]){
                ++curSameCount;
                
                if (curSameCount>= kLimitSameCount) break; //for curSameCount*kFixedFloatSmooth_base
                const TFixedFloatSmooth curSameRatio= (curSameCount*kFixedFloatSmooth_base)
                                                      /(length+kSmoothLength);
                if (curSameRatio>=curBestSameRatio){
                    curBestSameRatio=curSameRatio;
                    curBestLength=length;
                }
            }
        }
        if ((curBestSameRatio<kExtendMinSameRatio)||(curBestLength<=2)){
            curBestLength=0;
        }
        
        return curBestLength;
    }

//尝试延长覆盖区域.
static void extend_cover(TDiffData& diff){
    std::vector<TOldCover>&  cover=diff.cover;

    TInt lastNewPos=0;
    for (TInt i=0; i<(TInt)cover.size(); ++i) {
        TInt newPos_next=(TInt)(diff.newData_end-diff.newData);
        if (i+1<(TInt)cover.size())
            newPos_next=cover[i+1].newPos;
        TOldCover& curCover=cover[i];
        //向前延伸.
        TInt extendLength_front=getCanExtendLength(curCover.oldPos-1,curCover.newPos-1,
                                                   -1,lastNewPos,newPos_next,diff);
        if (extendLength_front>0){
            curCover.oldPos-=extendLength_front;
            curCover.newPos-=extendLength_front;
            curCover.length+=extendLength_front;
        }
        //向后延伸.
        TInt extendLength_back=getCanExtendLength(curCover.oldPos+curCover.length,
                                                  curCover.newPos+curCover.length,
                                                  1,lastNewPos,newPos_next,diff);
        if (extendLength_back>0){
            curCover.length+=extendLength_back;
        }

        lastNewPos=curCover.newPos+curCover.length;
    }
}

//用覆盖线得到差异数据.
static void sub_cover(TDiffData& diff){
    std::vector<TOldCover>& cover=diff.cover;
    const TByte*            newData=diff.newData;
    const TByte*            oldData=diff.oldData;

    TInt newPos_end=0;
    for (TInt i=0;i<(TInt)cover.size();++i){
        const TInt newPos=cover[i].newPos;
        if (newPos>newPos_end){
            diff.newDataDiff.insert(diff.newDataDiff.end(),newData+newPos_end,newData+newPos);
            diff.newDataSubDiff.resize(newPos,0);
        }
        const TInt oldPos=cover[i].oldPos;
        const TInt length=cover[i].length;
        for (TInt i=0; i<length;++i) {
            diff.newDataSubDiff.push_back(newData[i+newPos]-oldData[i+oldPos]);
        }
        newPos_end=newPos+length;
    }
    if (newPos_end<diff.newData_end-newData){
        diff.newDataDiff.insert(diff.newDataDiff.end(),newData+newPos_end,diff.newData_end);
        diff.newDataSubDiff.resize(diff.newData_end-newData,0);
    }
}

//diff结果序列化输出.
static void serialize_diff(const TDiffData& diff,std::vector<TByte>& out_serializeDiffStream){
    const TUInt ctrlCount=(TUInt)diff.cover.size();
    std::vector<TByte> length_buf;
    std::vector<TByte> inc_newPos_buf;
    std::vector<TByte> inc_oldPos_buf;
    {
        TInt oldPosBack=0;
        TInt newPos_end=0;
        for (TUInt i=0; i<ctrlCount; ++i) {
            packUInt(length_buf, diff.cover[i].length);
            assert(diff.cover[i].newPos>=newPos_end);
            packUInt(inc_newPos_buf,diff.cover[i].newPos-newPos_end); //save inc_newPos
            if (diff.cover[i].oldPos>=oldPosBack){ //save inc_oldPos
                packUIntWithTag(inc_oldPos_buf,diff.cover[i].oldPos-oldPosBack, 0, 1);
            }else{
                packUIntWithTag(inc_oldPos_buf,oldPosBack-diff.cover[i].oldPos, 1, 1);
            }
            oldPosBack=diff.cover[i].oldPos;
            newPos_end=diff.cover[i].newPos+diff.cover[i].length;
        }
    }

    packUInt(out_serializeDiffStream, ctrlCount);
    packUInt(out_serializeDiffStream, (TUInt)length_buf.size());
    packUInt(out_serializeDiffStream, (TUInt)inc_newPos_buf.size());
    packUInt(out_serializeDiffStream, (TUInt)inc_oldPos_buf.size());
    packUInt(out_serializeDiffStream, (TUInt)diff.newDataDiff.size());
    out_serializeDiffStream.insert(out_serializeDiffStream.end(),
                                   length_buf.begin(), length_buf.end());
    out_serializeDiffStream.insert(out_serializeDiffStream.end(),
                                   inc_newPos_buf.begin(), inc_newPos_buf.end());
    out_serializeDiffStream.insert(out_serializeDiffStream.end(),
                                   inc_oldPos_buf.begin(), inc_oldPos_buf.end());
    out_serializeDiffStream.insert(out_serializeDiffStream.end(),
                                   diff.newDataDiff.begin(), diff.newDataDiff.end());
    std::vector<TByte> rleData;
    const TByte* newDataSubDiff_begin=0;
    if (!diff.newDataSubDiff.empty())
        newDataSubDiff_begin=&diff.newDataSubDiff[0];
    bytesRLE_save(rleData,newDataSubDiff_begin,
                          newDataSubDiff_begin+diff.newDataSubDiff.size(),kRle_bestSize);
    out_serializeDiffStream.insert(out_serializeDiffStream.end(),
                                   rleData.begin(),rleData.end());
}

    struct THDiffPrivateParams{
        int kMinMatchLength;
        int kMinSingleMatchLength;
    };
    
}//end namespace


void __hdiff_private__create_diff(const TByte* newData,const TByte* newData_end,
                                  const TByte* oldData,const TByte* oldData_end,
                                  std::vector<TByte>& out_diff,const void* _kDiffParams,
                                  const TSuffixString* sstring=0){
    
    assert(newData<=newData_end);
    assert(oldData<=oldData_end);
    const THDiffPrivateParams& kDiffParams=*(const THDiffPrivateParams*)_kDiffParams;
    TSuffixString _sstring_default(0,0);
    if (sstring==0){
        _sstring_default.resetSuffixString(oldData,oldData_end);
        sstring=&_sstring_default;
    }
    
    TDiffData diff;
    diff.newData=newData;
    diff.newData_end=newData_end;
    diff.oldData=oldData;
    diff.oldData_end=oldData_end;
    
    search_cover(diff,*sstring,kDiffParams.kMinMatchLength);
    sstring=0;
    _sstring_default.clear();
    
    extend_cover(diff);//先尝试扩展.
    select_cover(diff,kDiffParams.kMinSingleMatchLength);
    extend_cover(diff);//select_cover会删除一些覆盖线,所以重新扩展.
    sub_cover(diff);
    serialize_diff(diff,out_diff);
}

void create_diff(const TByte* newData,const TByte* newData_end,
                 const TByte* oldData,const TByte* oldData_end,
                 std::vector<TByte>& out_diff){
    static const THDiffPrivateParams kDiffParams_default={
                                        8,   //最小搜寻覆盖长度. //二进制:7--9  文本: 9  zip文件:5-7
                                        24}; //最小独立覆盖长度. //二进制:8--12 文本:17-25 zip文件:6-9
    __hdiff_private__create_diff(newData,newData_end,
                                 oldData,oldData_end,out_diff,&kDiffParams_default);
}

bool check_diff(const TByte* newData,const TByte* newData_end,
                const TByte* oldData,const TByte* oldData_end,
                const TByte* diff,const TByte* diff_end){
    std::vector<TByte> testNewData(newData_end-newData);
    TByte* testNewData_begin=0;
    if (!testNewData.empty()) testNewData_begin=&testNewData[0];
    if (!patch(testNewData_begin,testNewData_begin+testNewData.size(),
               oldData,oldData_end,
               diff,diff_end))
        return false;
    for (TUInt i=0; i<(TUInt)testNewData.size(); ++i) {
        if (testNewData[i]!=newData[i])
            return false;
    }
    return true;
}


